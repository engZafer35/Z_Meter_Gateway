/******************************************************************************
* #Author       : Zafer Satilmis
* #Revision     : 1.0
* #Date         : 26 Mar 2024 - 16:09:29
* #File Name    : AppGsmManager.c
*******************************************************************************/
/******************************************************************************
*
*
*******************************************************************************/
#define DEBUG_LEVEL  (DEBUG_LEVEL_DEBUG)
/********************************* INCLUDES ***********************************/
#include "AppGsmManager.h"


#include "net_config.h"
#include "uart_driver.h"

#include "AppInternalMsgFrame.h"
#include "AppDataBus.h"

#include "MiddEventTimer.h"
/****************************** MACRO DEFINITIONS *****************************/

/******************************* TYPE DEFINITIONS *****************************/
typedef union
{
    struct
    {
        BOOL pppLinkState   : 1; //1: ppp link is ready, 0: ppp link is not ready
        BOOL modemState     : 1; //1:Modem is ready, 0 modem is not ready
        BOOL gsmSignalLevel : 7; //100 max signal quality, 0 no signal
    };

}GsmDataBusPacket;
/********************************** VARIABLES *********************************/
static S32 gs_gsmDbusID;
static GsmDataBusPacket gs_dataBusPck;
static S32 gs_timerId;

/***************************** STATIC FUNCTIONS  ******************************/
static void gsmTimerCb (void)
{
    DBUS_PACKET dbPacket;
    GsmMsg gsmMsg;

    DEBUG_INFO("Published Gsm Data");

    gsmMsg.modemStat   = gs_dataBusPck.modemState;
    gsmMsg.signalLevel = gs_dataBusPck.gsmSignalLevel;
    gsmMsg.connStat    = gs_dataBusPck.pppLinkState;

    dbPacket.packetID   = 0;
    dbPacket.pri        = EN_PRIORITY_MED;
    dbPacket.retainFlag = TRUE;
    dbPacket.topic      = EN_DBUS_TOPIC_GSM;

    appIntMsgCreateGsmMsg(&gsmMsg, dbPacket.payload.data, &dbPacket.payload.dataLeng);
    appDBusPublish(gs_gsmDbusID, &dbPacket);
}

#if USE_CYCLONE_LIB == 1

RETURN_STATUS modemSendAtCommand(NetInterface *interface, const char *command, char *response, U32 size)
{
    RETURN_STATUS retVal = FAILURE;
    error_t error;
    size_t n;

    DEBUG_INFO("\r\nAT command:  %s", command);

    //Send AT command
    error = pppSendAtCommand(interface, command);

   //Check status code
   if(!error)
   {
      n = 0;
      while(n < size)
      {
         //Wait for a response from the modem
         error = pppReceiveAtCommand(interface, response + n, size - n);

         if(error)
         {
            break;
         }

         //Status string received?
         if(strstr(response, "OK") ||
            strstr(response, "CONNECT") ||
            strstr(response, "RING") ||
            strstr(response, "NO CARRIER") ||
            strstr(response, "ERROR") ||
            strstr(response, "NO ANSWER"))
         {
             DEBUG_INFO("AT response: %s", response);
             retVal = SUCCESS;
             break;
         }
         //Update the length of the response
         n = strlen(response);
      }
   }

   return retVal;
}

RETURN_STATUS modemInit(NetInterface *interface)
{
    char buffer[128];
//    ping(&netInterface[0], &ip, 32, 0xFF, 2000000, NULL);

    //Set timeout for blocking operations
    pppSetTimeout(interface, APP_PPP_TIMEOUT);

    //Debug message
    DEBUG_INFO("Reseting modem...");
    //todo: run hw reset function here

    DEBUG_INFO("Initializing modem..");

    //Module identification
    if(SUCCESS != modemSendAtCommand(interface, "AT+CGMM\r", buffer, sizeof(buffer)))
    {
        return FAILURE;
    }

    //Get software version
    if(SUCCESS != modemSendAtCommand(interface, "AT+CGMR\r", buffer, sizeof(buffer)))
    {
        return FAILURE;
    }

    //Enable verbose mode
    if(SUCCESS != modemSendAtCommand(interface, "AT+CMEE=2\r", buffer, sizeof(buffer)))
    {
        return FAILURE;
    }

    //Set the functionality level (full fonctionnality)
    if(SUCCESS != modemSendAtCommand(interface, "AT+CFUN=1\r", buffer, sizeof(buffer)))
    {
        return FAILURE;
    }

    //Query the ICCID of the SIM card
    if(SUCCESS != modemSendAtCommand(interface, "AT+QCCID\r", buffer, sizeof(buffer)))
    {
        return FAILURE;
    }

    //Check if the SIM device needs the PIN code
    if(SUCCESS != modemSendAtCommand(interface, "AT+CPIN?\r", buffer, sizeof(buffer)))
    {
        return FAILURE;
    }

    //Check whether the PIN code is required
    if(strstr(buffer, "+CPIN: SIM PIN") != NULL)
    {
#ifdef APP_PPP_PIN_CODE
      //Format AT+CPIN command
      sprintf(buffer, "AT+CPIN=%s\r", APP_PPP_PIN_CODE);

      //Send AT command
      if(NO_ERROR != modemSendAtCommand(interface, buffer, buffer, sizeof(buffer)))
      {
          return FAILURE;
      }
#else
       DEBUG_INFO("PIN code is required!");
       return FAILURE;
    #endif
    }
    else if(strstr(buffer, "+CPIN: READY") != NULL)
    {
        DEBUG_INFO("The PIN code is not required\n");
    }

    //Enable network registration
    if(SUCCESS != modemSendAtCommand(interface, "AT+CREG=2\r", buffer, sizeof(buffer)))
    {
        return FAILURE;
    }

   //Wait for the module to be registered
   while(1)
   {
      //Check if the module is registered
      if(SUCCESS != modemSendAtCommand(interface, "AT+CREG?\r", buffer, sizeof(buffer)))
      {
          return FAILURE;
      }

      if(strstr(buffer, "+CREG: 2,0") != NULL)
      {
         //Not registered
      }
      else if(strstr(buffer, "+CREG: 2,1") != NULL)
      {
          //Registered (home network)
          break;
      }
      else if(strstr(buffer, "+CREG: 2,5") != NULL)
      {
          //Registered (roaming)
          break;
      }

       //Successful initialization
       osDelayTask(1000);
   }

   //Successful processing
   return SUCCESS;
}

RETURN_STATUS modemConnect(NetInterface *interface)
{
    char buffer[64];
    Ipv4Addr ipv4Addr;

    sprintf(buffer, "AT+CGDCONT=1,\"IP\",\"%s\"\r", APP_PPP_APN);

    if(SUCCESS != modemSendAtCommand(interface, buffer, buffer, sizeof(buffer)))
    {
       return FAILURE;
    }

    //Format ATD command
    sprintf(buffer, "ATD%s\r", APP_PPP_PHONE_NUMBER);

    if(SUCCESS != modemSendAtCommand(interface, buffer, buffer, sizeof(buffer)))
    {
        return FAILURE;
    }

    //Check response
    if(strstr(buffer, "NO CARRIER") != NULL)
    {
        return FAILURE;
    }
    else if(strstr(buffer, "CONNECT") == NULL)
    {
        return FAILURE;
    }

    //   //Clear local IPv4 address
    //   ipv4StringToAddr("0.0.0.0", &ipv4Addr);
    //   ipv4SetHostAddr(interface, ipv4Addr);
    //
    //   //Clear peer IPv4 address
    //   ipv4StringToAddr("0.0.0.0", &ipv4Addr);
    //   ipv4SetDefaultGateway(interface, ipv4Addr);

    //Set primary DNS server
    ipv4StringToAddr(APP_PPP_PRIMARY_DNS, &ipv4Addr);
    ipv4SetDnsServer(interface, 0, ipv4Addr);

    //Set secondary DNS server
    ipv4StringToAddr(APP_PPP_SECONDARY_DNS, &ipv4Addr);
    ipv4SetDnsServer(interface, 1, ipv4Addr);

    //Set username and password
    pppSetAuthInfo(interface, "", "");

    DEBUG_INFO("Establishing PPP connection...");

    //Establish a PPP connection
    if (NO_ERROR != pppConnect(interface))
    {
        return FAILURE;
    }

   return SUCCESS;
}

static void pppLinkStatusCb(NetInterface *interface, int linkState, void *param)
{
    DBUS_PACKET dbPacket;
    GsmMsg gsmMsg;

    //update link state and then share it over data bus
    gs_dataBusPck.pppLinkState = linkState;

    gsmMsg.modemStat    = gs_dataBusPck.modemState;
    gsmMsg.signalLevel  = gs_dataBusPck.gsmSignalLevel;
    gsmMsg.connStat     = gs_dataBusPck.pppLinkState;

    dbPacket.packetID   = 0;
    dbPacket.pri        = EN_PRIORITY_MED;
    dbPacket.retainFlag = TRUE;
    dbPacket.topic      = EN_DBUS_TOPIC_GSM;

    appIntMsgCreateGsmMsg(&gsmMsg, dbPacket.payload.data, &dbPacket.payload.dataLeng);
    appDBusPublish(gs_gsmDbusID, &dbPacket);
}

//static PppSettings pppSettings;
static PppContext pppContext;
#define APP_IF_NAME "PPP0"

/***************************** PUBLIC FUNCTIONS  ******************************/
RETURN_STATUS appGsmMngInit(void)
{
    RETURN_STATUS retVal = SUCCESS;
    NetInterface *interface;
    PppSettings pppSettings;

    //TCP/IP stack initialization
    if(NO_ERROR != netInit())
    {
        retVal = FAILURE;
        DEBUG_ERROR("Failed to initialize TCP/IP stack!");
    }

    if (SUCCESS == retVal)
    {
        //Configure the first network interface
        interface = &netInterface[0];

        //Get default PPP settings
        pppGetDefaultSettings(&pppSettings);
        //Select the underlying interface
        pppSettings.interface = interface;
        //Default async control character map
        pppSettings.accm = 0x00000000;
        //Allowed authentication protocols
        pppSettings.authProtocol = PPP_AUTH_PROTOCOL_PAP | PPP_AUTH_PROTOCOL_CHAP_MD5;

        //Initialize PPP
        if(NO_ERROR != pppInit(&pppContext, &pppSettings))
        {
            retVal = FAILURE;
            DEBUG_ERROR("Failed to initialize PPP!");
        }

        if (SUCCESS == retVal)
        {
            //Set interface name
            netSetInterfaceName(interface, APP_IF_NAME);
            //Select the relevant UART driver
            netSetUartDriver(interface, &uartDriver);

            //Initialize network interface
            if(NO_ERROR != netConfigInterface(interface))
            {
                retVal = FAILURE;
                DEBUG_ERROR("Failed to configure interface %s!", interface->name);
            }
        }
    }

    if (SUCCESS == retVal)
    {
        retVal = FAILURE;
        //Modem initialization
        if(SUCCESS == modemInit(interface))
        {
            netAttachLinkChangeCallback(&netInterface[0], pppLinkStatusCb, NULL);
            retVal  = appDBusRegister(EN_DBUS_TOPIC_DEVICE, &gs_gsmDbusID);
            retVal |= middEventTimerRegister(&gs_timerId, gsmTimerCb, WAIT_10_SEC , TRUE);
            middEventTimerStart(gs_timerId);

            gs_dataBusPck.modemState = TRUE;
        }
    }

    return retVal;
}

RETURN_STATUS appGsmMngOpenPPP(void)
{
    RETURN_STATUS retVal;

    if (SUCCESS == modemConnect(&netInterface[0]))
    {
        retVal = SUCCESS;
        gs_dataBusPck.pppLinkState = TRUE;
    }

    return retVal;
}

RETURN_STATUS appGsmMngClosePPP(void)
{
   RETURN_STATUS retVal = FAILURE;

   if (FALSE == isPPPWatingRxTxData())
   {
       DEBUG_INFO("Closing PPP connection...");

       if (NO_ERROR == pppClose(&netInterface[0]))
       {
           retVal = SUCCESS;
           gs_dataBusPck.pppLinkState = FALSE;
       }
   }
   return retVal;
}

BOOL appGsmMngIsNetworkReady(void)
{
    return gs_dataBusPck.pppLinkState;
}


#else

RETURN_STATUS appGsmMngInit(void)
{
    RETURN_STATUS retVal = SUCCESS;

    retVal  = appDBusRegister(EN_DBUS_TOPIC_DEVICE, &gs_gsmDbusID);
    retVal |= middEventTimerRegister(&gs_timerId, gsmTimerCb, WAIT_5_SEC , TRUE);
    middEventTimerStart(gs_timerId);

    gs_dataBusPck.modemState = TRUE;
    gs_dataBusPck.gsmSignalLevel = 80; //it is updated jus for test, it does not matter in PC

    return retVal;
}

RETURN_STATUS appGsmMngOpenPPP(void)
{
    /* This info will be shared over DBus by timerCB*/
    gs_dataBusPck.pppLinkState = TRUE; // In PC, eth connection is ready every time.
    return SUCCESS;
}

RETURN_STATUS appGsmMngClosePPP(void)
{
    /* This info will be shared over DBus by timerCB*/
    gs_dataBusPck.pppLinkState = FALSE;
    return SUCCESS;
}

BOOL appGsmMngIsNetworkReady(void)
{
    return gs_dataBusPck.pppLinkState;
}

/* This function will be used just test purpose, manually break connection */
void appGsmMngSetConnStat(BOOL stat)
{
    gs_dataBusPck.pppLinkState = stat;
    if (stat)
    {
        gs_dataBusPck.modemState     = TRUE;
        gs_dataBusPck.gsmSignalLevel = 75;
        gs_dataBusPck.pppLinkState   = TRUE;
    }
    else
    {
        gs_dataBusPck.modemState     = FALSE;
        gs_dataBusPck.gsmSignalLevel = 0;
        gs_dataBusPck.pppLinkState   = FALSE;;
    }
}

#endif


/******************************** End Of File *********************************/
