#ifndef _GLOBALS_H
   #define _GLOBALS_H

   // globals
   const char okStr[] = {"OK"};
   const char connectStr[] = {"CONNECT"};
   const char ringStr[] = {"RING"};
   const char noCarrierStr[] = {"NO CARRIER"};
   const char errorStr[] = {"ERROR"};
   const char noAnswerStr[] = {"NO ANSWER"};
   enum ResultCodes { R_OK, R_CONNECT, R_RING, R_NO_CARRIER, R_ERROR, R_NO_ANSWER, R_RING_IP };
   const char * const resultCodes[] = { okStr, connectStr, ringStr, noCarrierStr, errorStr, noAnswerStr, ringStr};
   enum DtrStates { DTR_IGNORE, DTR_GOTO_COMMAND, DTR_END_CALL, DTR_RESET};

   typedef struct Settings {
      uint16_t      magicNumber;
      char          ssid[MAX_SSID_LEN + 1];
      char          wifiPassword[MAX_WIFI_PWD_LEN + 1];
      uint32_t      serialSpeed;
      uint8_t       dataBits;
      uart_parity_t parity;
      uint8_t       stopBits;
      bool          rtsCts;
      uint8_t       width, height;
      char          escChar;
      char          alias[SPEED_DIAL_SLOTS][MAX_ALIAS_LEN + 1];
      char          speedDial[SPEED_DIAL_SLOTS][MAX_SPEED_DIAL_LEN + 1];
      char          mdnsName[MAX_MDNSNAME_LEN + 1];
      uint8_t       autoAnswer;
      uint16_t      listenPort;
      char          busyMsg[MAX_BUSYMSG_LEN + 1];
      char          serverPassword[MAX_PWD_LEN + 1];
      bool          echo;
      uint8_t       telnet;
      char          autoExecute[MAX_AUTOEXEC_LEN + 1];
      char          terminal[MAX_TERMINAL_LEN + 1];
      char          location[MAX_LOCATION_LEN + 1];
      bool          startupWait;
      bool          extendedCodes;
      bool          verbose;
      bool          quiet;
      DtrStates     dtrHandling;
   } SETTINGS_T;
   
   typedef struct TCP_CLIENT_T_ {
      struct tcp_pcb *pcb;
      ip_addr_t remoteAddr;
      bool connected;
      bool connectFinished;
      volatile bool sending;
      uint8_t rxBuff[TCP_CLIENT_RX_BUF_SIZE];
      volatile uint16_t rxBuffLen;
      uint16_t rxBuffHead;
      volatile uint16_t rxBuffTail;
      volatile uint16_t totLen;
      uint8_t txBuff[TCP_CLIENT_TX_BUF_SIZE];
      volatile uint16_t txBuffLen;
      volatile uint16_t txBuffHead;
      uint16_t txBuffTail;
   } TCP_CLIENT_T;
   
   typedef struct TCP_SERVER_T_ {
      struct tcp_pcb *pcb;
      struct tcp_pcb *clientPcb;
   } TCP_SERVER_T;
   
   SETTINGS_T settings;
   TCP_CLIENT_T *tcpClient,tcpClient0,tcpDroppedClient;
   TCP_SERVER_T tcpServer;
   // incantation to switch from line mode to character mode
   const uint8_t toCharModeMagic[] = {IAC,WILL,SUP_GA,IAC,WILL,ECHO,IAC,WONT,LINEMODE};
   uint32_t bytesIn = 0, bytesOut = 0;
   unsigned long connectTime = 0;
   char atCmd[MAX_CMD_LEN + 1], lastCmd[MAX_CMD_LEN + 1];
   unsigned atCmdLen = 0;
   enum {CMD_NOT_IN_CALL, CMD_IN_CALL, ONLINE, PASSWORD} state = CMD_NOT_IN_CALL;
   bool     ringing = false;     // no incoming call
   uint8_t  ringCount = 0;       // current incoming call ring count
   uint32_t nextRingMs = 0;      // time of mext RING result
   uint8_t  escCount = 0;        // Go to AT mode at "+++" sequence, that has to be counted
   uint32_t guardTime = 0;       // When did we last receive a "+++" sequence
   char     password[MAX_PWD_LEN + 1];
   uint8_t  passwordTries = 0;   // # of unsuccessful tries at incoming password
   uint8_t  passwordLen = 0;
   uint8_t  txBuf[TX_BUF_SIZE];  // Transmit Buffer
   uint8_t  sessionTelnetType;
   volatile bool dtrWentInactive = false;
   bool     amClient = false;    // true if we've connected TO a remote server
#ifndef NDEBUG
   uint16_t maxTotLen = 0;
   uint16_t maxRxBuffLen = 0;
   uint16_t maxTxBuffLen = 0;
#endif

#endif
