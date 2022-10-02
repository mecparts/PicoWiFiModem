volatile bool dnsLookupFinished = false;

void dnsLookupDone(const char *name, const ip_addr_t *ipaddr, void *arg) {
   ip_addr_t *resolved = (ip_addr_t *)arg;
   if( ipaddr && ipaddr->addr) {
      resolved->addr = ipaddr->addr;
   }
   dnsLookupFinished = true;
}

bool dnsLookup(const char *name, ip_addr_t *resolved) {
   
   dnsLookupFinished = false;
   ip4_addr_set_any(resolved);
   
   switch( dns_gethostbyname(name, resolved, dnsLookupDone, resolved) ) {
      case ERR_OK:
         return true;
         break;
      case ERR_INPROGRESS:
         break;
      default:
         return false;
   }
   while( !dnsLookupFinished ) {
      tight_loop_contents();
   }
   return !ip4_addr_isany(resolved);
}

uint32_t millis(void) {
   return to_ms_since_boot(get_absolute_time());
}

bool tcpIsConnected(TCP_CLIENT_T *client) {
   if( client && client->pcb && client->pcb->callback_arg ) {
      return client->connected;
   }
   return false;
}

err_t tcpClientClose(TCP_CLIENT_T *client) {
   err_t err = ERR_OK;
   
   if( client ) {
      client->connected = false;
      if( client->pcb ) {
         cyw43_arch_lwip_begin();
         tcp_err( client->pcb, NULL);
         tcp_sent(client->pcb, NULL);
         tcp_recv(client->pcb, NULL);
         tcp_arg( client->pcb, NULL);
         err = tcp_close(client->pcb);
         if( err != ERR_OK ) {
            tcp_abort(client->pcb);
            err = ERR_ABRT;
         }
         cyw43_arch_lwip_end();
         client->pcb = NULL;
      }
   }
   return err;
}

// NB: the PCB may have already been freed when this function is called
void tcpClientErr(void *arg, err_t err) {
   TCP_CLIENT_T *client = (TCP_CLIENT_T *)arg;

   if( client ) {
      client->connectFinished = true;
      client->connected = false;
      client->pcb = NULL;
   }
}

err_t tcpSend(TCP_CLIENT_T *client) {
   err_t err = ERR_OK;
   int maxLen = tcp_sndbuf(client->pcb);
   if( client->txBuffLen < maxLen ) {
      maxLen = client->txBuffLen;
   }
   uint8_t tmp[maxLen];
   if( tmp ) {
      for( int i=0; i<maxLen; ++i ) {
         tmp[i] = client->txBuff[client->txBuffHead++];
         if( client->txBuffHead == TCP_CLIENT_TX_BUF_SIZE ) {
            client->txBuffHead = 0;
         }
         --client->txBuffLen;
      }
      client->sending = true;
      cyw43_arch_lwip_begin();
      err = tcp_write(client->pcb, tmp, maxLen, TCP_WRITE_FLAG_COPY);
      client->sending = err == ERR_OK;
      tcp_output(client->pcb);
      cyw43_arch_lwip_end();
      if( err != ERR_OK ) {
         printf("{%d}",err);
      }
   }
   return err;
}

err_t tcpSent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
   TCP_CLIENT_T *client = (TCP_CLIENT_T *)arg;
   err_t err = ERR_OK;
   
   if( client->txBuffLen ) {
      err = tcpSend(client);
   } else {
      client->sending = false;
   }
   return err;
}

err_t tcpRecv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
   TCP_CLIENT_T *client = (TCP_CLIENT_T *)arg;

   if( !p ) {
      return tcpClientClose(client);
   }
   if( p->tot_len > 0 && client ) {
      for( struct pbuf *q = p; q && client->rxBuffLen < TCP_CLIENT_RX_BUF_SIZE; q = q->next ) {
         for( int i=0; i<q->len && client->rxBuffLen < TCP_CLIENT_RX_BUF_SIZE; ++i ) {
            client->rxBuff[client->rxBuffTail++] = ((uint8_t *)q->payload)[i];
            if( client->rxBuffTail == TCP_CLIENT_RX_BUF_SIZE ) {
               client->rxBuffTail = 0;
            }
            ++client->rxBuffLen;
         }
      }
   }
   cyw43_arch_lwip_begin();
   tcp_recved(tpcb, p->tot_len);
   cyw43_arch_lwip_end();
   pbuf_free(p);
   return ERR_OK;
}

err_t tcpHasConnected(void *arg, struct tcp_pcb *tpcb, err_t err) {
   TCP_CLIENT_T *client = (TCP_CLIENT_T*)arg;
   
   client->connectFinished = true;
   client->connected = err == ERR_OK;
   if( err != ERR_OK ) {
      tcpClientClose(client);
   }
   return ERR_OK;
}

TCP_CLIENT_T *tcpConnect(TCP_CLIENT_T *client, const char *host, int portNum) {
   if( !dnsLookup(host, &client->remoteAddr) ) {
      return NULL;
   } else {
      client->pcb = tcp_new_ip_type(IP_GET_TYPE(client->remoteAddr));
      if( !client->pcb ) {
         return NULL;
      }
   }
   tcp_arg( client->pcb, client);
   tcp_recv(client->pcb, tcpRecv);
   tcp_sent(client->pcb, tcpSent);
   tcp_err( client->pcb, tcpClientErr);
   tcp_nagle_disable(client->pcb);  // disable Nalge algorithm by default

   client->rxBuffLen = 0;
   client->rxBuffHead = 0;
   client->rxBuffTail = 0;

   client->txBuffLen = 0;
   client->txBuffHead = 0;
   client->txBuffTail = 0;

   client->connected = false;
   client->connectFinished = false;
   client->sending = false;

   cyw43_arch_lwip_begin();
   err_t err = tcp_connect(client->pcb, &client->remoteAddr, portNum, tcpHasConnected);
   cyw43_arch_lwip_end();
   
   if( err != ERR_OK ) {
      client->pcb = NULL;
      return NULL;
   }
   
   while( client->pcb && client->pcb->callback_arg && !client->connectFinished && !uart_is_readable(uart0)) {
      tight_loop_contents();
   }
   if( !client->connected ) {
      client->pcb = NULL;
      return NULL;
   }
   return client;
}

// NB: the PCB may have already been freed when this function is called
void tcpServerErr(void *arg, err_t err) {
   TCP_SERVER_T *server = (TCP_SERVER_T *)arg;

   if( server ) {
      server->pcb = NULL;
      server->clientPcb = NULL;
   }
}

err_t tcpServerAccept(void *arg, struct tcp_pcb *clientPcb, err_t err) {
   TCP_SERVER_T *server = (TCP_SERVER_T*)arg;

   if( err != ERR_OK || !clientPcb ) {
      printf("Failure in accept: %d\n",err);
      tcp_close(server->pcb);
      return ERR_VAL;
   }
   if( server->clientPcb ) {
      printf("Overwriting server->clientPcb\n");
   }
   server->clientPcb = clientPcb;
   return ERR_OK;
}

bool tcpServerStart(TCP_SERVER_T *server, int portNum) {
   server->pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
   if( !server->pcb ) {
      return false;
   }

   if( tcp_bind(server->pcb, NULL, portNum) != ERR_OK ) {
      return false;
   }

   server->clientPcb = NULL;

   struct tcp_pcb *pcb = tcp_listen_with_backlog(server->pcb, 1);
   if( !pcb ) {
      if( server->pcb ) {
         tcp_close(server->pcb);
         server->pcb = NULL;
      }
      return false;
   }
   server->pcb = pcb;

   tcp_arg(   server->pcb, server);
   tcp_accept(server->pcb, tcpServerAccept);
   tcp_err(   server->pcb, tcpClientErr);

   return true;
}

uint16_t tcpWriteBuf(TCP_CLIENT_T *client, const uint8_t *buf, uint16_t len) {
   if( client && client->pcb && client->pcb->callback_arg) {

      for( uint16_t i=0; i<len; ++i ) {
         while( client->txBuffLen >= TCP_CLIENT_TX_BUF_SIZE && client->connected ) {
            tight_loop_contents();
         }
         client->txBuff[client->txBuffTail++] = buf[i];
         if( client->txBuffTail == TCP_CLIENT_TX_BUF_SIZE ) {
            client->txBuffTail = 0;
         }
         ++client->txBuffLen;
      }
      if( client->txBuffLen && client->pcb && client->pcb->callback_arg && !client->sending ) {
         tcpSend(client);
      }
      return len;
   }
   return 0;
}

uint16_t tcpWriteStr(TCP_CLIENT_T *client, const char *str) {
   return tcpWriteBuf(client, (uint8_t *)str, strlen(str));
}

uint16_t tcpWriteByte(TCP_CLIENT_T *client, uint8_t c) {
   return tcpWriteBuf(client, (uint8_t *)&c, 1);
}

uint16_t tcpBytesAvailable(TCP_CLIENT_T *client) {
   if( client ) {
      return client->rxBuffLen;
   }
   return 0;
}

int tcpReadByte(TCP_CLIENT_T *client, int rqstTimeout = -1) {
   int c;
   uint32_t timeout = 0;
   
   if( client ) {
      if( rqstTimeout > 0 ) {
         timeout = millis() + rqstTimeout;
      }
      do {
         if( client->rxBuffLen ) {
            c = client->rxBuff[client->rxBuffHead++];
            if( client->rxBuffHead == TCP_CLIENT_RX_BUF_SIZE ) {
               client->rxBuffHead = 0;
            }
            client->rxBuffLen--;
            return c;
         } else {
            tight_loop_contents();
         }
      } while( timeout > millis() );
   }
   return -1;
}

uint16_t tcpReadBytesUntil(TCP_CLIENT_T *client, uint8_t terminator, char *buf, uint16_t max_len) {
   char *p = buf;
   uint16_t c;
   
   uint32_t timeout = millis() + 1000;
   if( max_len > 0 ) {
      do {
         if( client->rxBuffLen ) {
            c = tcpReadByte(client);
            if( c != terminator ) {
               *p++ = (char)c;
               --max_len;
               timeout = millis() + 1000;
            } else {
               break;
            }
         } else {
            tight_loop_contents();
         }
      } while( max_len > 0 && timeout > millis() );
      return p - buf;
   }
   return 0;
}

void tcpTxFlush(TCP_CLIENT_T *client) {
   if( client ) {
      while( client->pcb && client->connected && client->txBuffLen ) {
         tight_loop_contents();
      }
   }
}

bool serverHasClient(TCP_SERVER_T *server) {
   return server->clientPcb != NULL;
}

TCP_CLIENT_T *serverGetClient(TCP_SERVER_T *server, TCP_CLIENT_T *client) {
   client->pcb = server->clientPcb;
   server->clientPcb = NULL;
   
   client->rxBuffLen = 0;
   client->rxBuffHead = 0;
   client->rxBuffTail = 0;

   client->txBuffLen = 0;
   client->txBuffHead = 0;
   client->txBuffTail = 0;

   client->sending = false;

   tcp_arg( client->pcb, client);
   tcp_err( client->pcb, tcpClientErr);
   tcp_sent(client->pcb, tcpSent);
   tcp_recv(client->pcb, tcpRecv);
   tcp_nagle_disable(client->pcb);  // disable Nalge algorithm by default

   client->connected = true;
   client->connectFinished = true;

   return client;
}
