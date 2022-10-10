//
//   Pico WiFi modem: a Pico W based RS232<->WiFi modem
//   with Hayes style AT commands and blinking LEDs.
//
//   A "let's learn about the Pico W and lwIP" project. It
//   betrays its ESP8266 + Arduino IDE roots in its
//   structure; certainly no one would start a Pico
//   project from scratch and write it this way!
//
//   Originally based on
//   Original Source Copyright (C) 2016 Jussi Salin <salinjus@gmail.com>
//   Additions (C) 2018 Daniel Jameson, Stardot Contributors
//   Additions (C) 2018 Paul Rickards <rickards@gmail.com>
//   Additions 2020-2022 Wayne Hortensius
//
//   This program is free software: you can redistribute it and/or modify
//   it under the tertms of the GNU General Public License as published by
//   the Free Software Foundation, either version 3 of the License, or
//   (at your option) any later version.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <string.h>
#include <time.h>
#include <malloc.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "hardware/i2c.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"


#include "wifi_modem.h"
#include "globals.h"
#include "eeprom.h"
#include "tcp_support.h"
#include "support.h"
#include "at_basic.h"
#include "at_extended.h"
#include "at_proprietary.h"

// =============================================================
void setup(void) {
   bool ok = true;

   stdio_init_all();

   gpio_set_function(CTS, GPIO_FUNC_UART);

   gpio_set_function(RTS, GPIO_FUNC_UART);

   gpio_init(DTR);
   gpio_set_dir(DTR, INPUT);

   gpio_init(RI);
   gpio_set_dir(RI, OUTPUT);
   gpio_put(RI, !ACTIVE);           // not ringing

   gpio_init(DCD);
   gpio_set_dir(DCD, OUTPUT);
   gpio_put(DCD, !ACTIVE);          // not connected

   gpio_init(DSR);
   gpio_set_dir(DSR, OUTPUT);
   gpio_put(DSR, !ACTIVE);          // modem is not ready
#ifndef NDEBUG
   gpio_init(TCP_WRITE_ERR);
   gpio_set_dir(TCP_WRITE_ERR, OUTPUT);
   gpio_put(TCP_WRITE_ERR, LOW);
   
   gpio_init(RXBUFF_OVFL);
   gpio_set_dir(RXBUFF_OVFL, OUTPUT);
   gpio_put(RXBUFF_OVFL, LOW);

   gpio_init(TXBUFF_OVFL);
   gpio_set_dir(TXBUFF_OVFL, OUTPUT);
   gpio_put(TXBUFF_OVFL, LOW);
#endif
   initEEPROM();
   readSettings(&settings);

   if( settings.magicNumber != MAGIC_NUMBER ) {
      // no valid data in EEPROM/NVRAM, populate with defaults
      factoryDefaults(NULL);
   }
   sessionTelnetType = settings.telnet;

   uart_set_baudrate(uart0, settings.serialSpeed);
   uart_set_format(uart0, settings.dataBits, settings.stopBits, settings.parity);
   uart_set_translate_crlf(uart0, false);
   setHardwareFlow(settings.rtsCts);

   // enable interrupt when DTR goes inactive if we're not ignoring it
   gpio_set_irq_enabled_with_callback(DTR, GPIO_IRQ_EDGE_RISE, settings.dtrHandling != DTR_IGNORE, dtrIrq );

   if( settings.startupWait ) {
      while( true ) {            // wait for a CR
         if( uart_is_readable(uart0) ) {
            if( uart_getc(uart0) == CR ) {
               break;
            }
         }
      }
   }

   cyw43_arch_init();
   cyw43_arch_enable_sta_mode();
   if( settings.ssid[0] ) {
      cyw43_arch_wifi_connect_timeout_ms(settings.ssid, settings.wifiPassword, CYW43_AUTH_WPA2_AES_PSK, 10000);
   }

   if( settings.listenPort ) {
      tcpServerStart(&tcpServer, settings.listenPort);
   }

#ifdef OTA_UPDATE_ENABLED
   if( settings.ssid[0] && cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP ) {
      setupOTAupdates();
   }
#endif

   if( cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP || !settings.ssid[0] ) {
      if( cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP ) {
         gpio_put(DSR, ACTIVE);  // modem is finally ready or SSID not configured
         dns_init();
      }
      if( settings.autoExecute[0] ) {
         strncpy(atCmd, settings.autoExecute, MAX_CMD_LEN);
         atCmd[MAX_CMD_LEN] = NUL;
         if( settings.echo ) {
            printf("%s\r\n", atCmd);
         }
         doAtCmds(atCmd);                  // auto execute command
      } else {
         sendResult(R_OK);
      }
   } else {
      sendResult(R_ERROR);           // SSID configured, but not connected
   }
}

// =============================================================
void loop(void) {

   checkForIncomingCall();

   if( settings.dtrHandling == DTR_RESET && checkDtrIrq() ) {
      resetToNvram(NULL);
   }

   switch( state ) {

      case CMD_NOT_IN_CALL:
#ifdef OTA_UPDATE_ENABLED
         if( cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP ) {
            ArduinoOTA.handle();
         }
#endif
         inAtCommandMode();
         break;

      case CMD_IN_CALL:
         inAtCommandMode();
         if( state == CMD_IN_CALL && !tcpIsConnected(tcpClient) ) {
            endCall();                    // hang up if not in a call
         }
         break;

      case PASSWORD:
         inPasswordMode();
         break;

      case ONLINE:
         if( uart_is_readable(uart0) ) {       // data from RS-232 to Wifi
            sendSerialData();
         }

         while( tcpBytesAvailable(tcpClient) && !uart_is_readable(uart0) ) { 
            // data from WiFi to RS-232
            int c = receiveTcpData();
            if( c != -1 ) {
               uart_putc_raw(uart0, (char)c);
            }
         }

         if( escCount == ESC_COUNT && millis() > guardTime ) {
            state = CMD_IN_CALL;          // +++ detected, back to command mode
            sendResult(R_OK);
            escCount = 0;
         }

         if( settings.dtrHandling != DTR_IGNORE && checkDtrIrq() ) {
            switch( settings.dtrHandling ) {
               
               case DTR_GOTO_COMMAND:
                  state = CMD_IN_CALL;
                  sendResult(R_OK);
                  escCount = 0;
                  break;
                  
               case DTR_END_CALL:
                  endCall();
                  break;
                  
               case DTR_RESET:
                  resetToNvram(NULL);
                  break;
            }
         }

         if( !tcpIsConnected(tcpClient) ) {   // no client?
            endCall();                        // then hang up
         }
         break;
   }
}

// =============================================================
void doAtCmds(char *atCmd) {
   size_t len;

   trim(atCmd);               // get rid of leading and trailing spaces
   if( atCmd[0] ) {
      // is it an AT command?
      if( strncasecmp(atCmd, "AT", 2) ) {
         sendResult(R_ERROR); // nope, time to die
      } else {
         // save command for possible future A/
         strncpy(lastCmd, atCmd, MAX_CMD_LEN);
         lastCmd[MAX_CMD_LEN] = NUL;
         atCmd += 2;          // skip over AT prefix
         len = strlen(atCmd);

         if( !atCmd[0] ) {
            // plain old AT
            sendResult(R_OK);
         } else {
            trim(atCmd);
            while( atCmd[0] ) {
               if( !strncasecmp(atCmd, "?", 1)  ) { // help message
                  // help
                  atCmd = showHelp(atCmd + 1);
               } else if( !strncasecmp(atCmd, "$SB", 3) ) {
                  // query/set serial speed
                  atCmd = doSpeedChange(atCmd + 3);
               } else if( !strncasecmp(atCmd, "$SU", 3) ) {
                  // query/set serial data configuration
                  atCmd = doDataConfig(atCmd + 3);
               } else if( !strncasecmp(atCmd, "$SSID", 5) ) {
                  // query/set WiFi SSID
                  atCmd = doSSID(atCmd + 5);
               } else if( !strncasecmp(atCmd, "$PASS", 5) ) {
                  // query/set WiFi password
                  atCmd = doWiFiPassword(atCmd + 5);
               } else if( !strncasecmp(atCmd, "C", 1) ) {
                  // connect/disconnect to WiFi
                  atCmd = wifiConnection(atCmd + 1);
               } else if( !strncasecmp(atCmd, "D", 1) && len > 2 && strchr("TPI", toupper(atCmd[1])) ) {
                  // dial a number
                  atCmd = dialNumber(atCmd + 2);
               } else if( !strncasecmp(atCmd, "DS", 2) && len == 3 ) {
                  // speed dial a number
                  atCmd = speedDialNumber(atCmd + 2);
               } else if( !strncasecmp(atCmd, "H", 1) || !strncasecmp(atCmd, "H0", 2) ) {
                  // hang up call
                  atCmd = hangup(atCmd + 1);
               } else if( !strncasecmp(atCmd, "&Z", 2) && isdigit(atCmd[2]) ) {
                  // speed dial query or set
                  atCmd = doSpeedDialSlot(atCmd + 2);
               } else if( !strncasecmp(atCmd, "O", 1) ) {
                  // go online
                  atCmd = goOnline(atCmd + 1);
               } else if( !strncasecmp(atCmd, "GET", 3) ) {
                  // get a web page (http only, no https)
                  atCmd = httpGet(atCmd + 3);
               } else if( settings.listenPort && !strncasecmp(atCmd, "A", 1) && serverHasClient(&tcpServer) ) {
                  // manually answer incoming connection
                  atCmd = answerCall(atCmd + 1);
               } else if( !strncasecmp(atCmd, "S0", 2) ) {
                  // query/set auto answer
                  atCmd = doAutoAnswerConfig(atCmd + 2);
               } else if( !strncasecmp(atCmd, "S2", 2) ) {
                  // query/set escape character
                  atCmd = doEscapeCharConfig(atCmd + 2);
               } else if( !strncasecmp(atCmd, "$SP", 3) ) {
                  // query set inbound TCP port
                  atCmd = doServerPort(atCmd + 3);
               } else if( !strncasecmp(atCmd, "$BM", 3) ) {
                  // query/set busy message
                  atCmd = doBusyMessage(atCmd + 3);
               } else if( !strncasecmp(atCmd, "&R", 2) ) {
                  // query/set require password
                  atCmd = doServerPassword(atCmd + 2);
               } else if( !strncasecmp(atCmd, "I", 1) ) {
                  // show network information
                  atCmd = showNetworkInfo(atCmd + 1);
               } else if( !strncasecmp(atCmd, "Z", 1) ) {
                  // reset to NVRAM
                  atCmd = resetToNvram(atCmd + 1);
               } else if( !strncasecmp(atCmd, "&V", 2) ) {
                  // display current and stored settings
                  atCmd = displayAllSettings(atCmd + 2);
               } else if( !strncasecmp(atCmd, "&W", 2) ) {
                  // write settings to EEPROM
                  atCmd = updateNvram(atCmd + 2);
               } else if( !strncasecmp(atCmd, "&D", 2) ) {
                  // DTR transition handling
                  atCmd = doDtrHandling(atCmd + 2);
               } else if( !strncasecmp(atCmd, "&F", 2) ) {
                  // factory defaults
                  atCmd = factoryDefaults(atCmd);
               } else if( !strncasecmp(atCmd, "E", 1) ) {
                  // query/set command mode echo
                  atCmd = doEcho(atCmd + 1);
               } else if( !strncasecmp(atCmd, "Q", 1) ) {
                  // query/set quiet mode
                  atCmd = doQuiet(atCmd + 1);
               } else if( !strncasecmp(atCmd, "RD", 2)
                       || !strncasecmp(atCmd, "RT", 2) ) {
                  // read time and date
                  atCmd = doDateTime(atCmd + 2);
               } else if( !strncasecmp(atCmd, "V", 1) ) {
                  // query/set verbose mode
                  atCmd = doVerbose(atCmd + 1);
               } else if( !strncasecmp(atCmd, "X", 1) ) {
                  // query/set extended result codes
                  atCmd = doExtended(atCmd + 1);
               } else if( !strncasecmp(atCmd, "$W", 2) ) {
                  // query/set startup wait
                  atCmd = doStartupWait(atCmd + 2);
               } else if( !strncasecmp(atCmd, "NET", 3) ) {
                  // query/set telnet mode
                  atCmd = doTelnetMode(atCmd + 3);
               } else if( !strncasecmp(atCmd, "$AE", 3) ) {
                  // do auto execute commands
                  atCmd = doAutoExecute(atCmd + 3);
               } else if( !strncasecmp(atCmd, "$TTY", 4) ) {
                  // do telnet terminal type
                  atCmd = doTerminalType(atCmd + 4);
               } else if( !strncasecmp(atCmd, "$TTL", 4) ) {
                  // do telnet location
                  atCmd = doLocation(atCmd + 4);
               } else if( !strncasecmp(atCmd, "$TTS", 4) ) {
                  // do telnet location
                  atCmd = doWindowSize(atCmd + 4);
               } else if( !strncasecmp(atCmd, "&K", 2) ) {
                  // do RTS/CTS flow control
                  atCmd = doFlowControl(atCmd + 2);
               } else if( !strncasecmp(atCmd, "$MDNS", 5) ) {
                  // handle mDNS name
                  atCmd = doMdnsName(atCmd + 5);
               } else {
                  // unrecognized command
                  sendResult(R_ERROR);
               }
               trim(atCmd);
            }
         }
      }
   }
}

int main(void) {
   setup();
   while( true ) {
      loop();
   }
   return 0;
}

