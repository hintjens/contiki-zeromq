/*
 * Copyright (c) 2012, Thingsquare, http://www.thingsquare.com/.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "contiki-net.h"
#include "net/ip/uip-debug.h"
#include "sys/cc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UIP_IP_BUF   ((struct uip_tcpip_hdr *)&uip_buf[UIP_LLH_LEN])

#define SERVER_PORT 9999

static void print_data(const uint8_t *data, int data_size);
struct pt hdl_conn_pt;
/********/
#ifndef ZMTP_MAX_CONNECTIONS
  #define ZMTP_MAX_CONNECTIONS 10
#endif

#ifndef ZMTP_MAX_ENDPOINTS
  #define ZMTP_MAX_ENDPOINTS 2
#endif

#ifndef ZMTP_INPUT_BUFFER_SIZE
  #define ZMTP_INPUT_BUFFER_SIZE 100
#endif

struct zmtp_connection {
  struct psock ps;
  uint8_t buffer[ZMTP_INPUT_BUFFER_SIZE];
  struct uip_conn *ip_conn;
};

typedef struct zmtp_connection zmtp_connection_t;
MEMB(zmtp_connections, zmtp_connection_t, ZMTP_MAX_CONNECTIONS);

struct zmtp_tcp_endpoint {
    uip_ipaddr_t addr;
    uint16_t port;
};
typedef struct zmtp_tcp_endpoint zmtp_tcp_endpoint_t;
MEMB(zmtp_tcp_endpoints, zmtp_tcp_endpoint_t, ZMTP_MAX_ENDPOINTS);

zmtp_tcp_endpoint_t *
zmtp_tcp_endpoint_new (uip_ipaddr_t *ip_addr, uint16_t port)
{
    zmtp_tcp_endpoint_t *self = memb_alloc(&zmtp_tcp_endpoints);
    if (self == NULL) {
        printf("Reached exhaustion of zmtp_tcp_endpoints");
        return NULL;
    }

    if(ip_addr != NULL)
        uip_ipaddr_copy(&self->addr, ip_addr);

    self->port = uip_htons(port);

    return self;
}

void
zmtp_tcp_endpoint_destroy (zmtp_tcp_endpoint_t **self_p)
{
    if (*self_p) {
        zmtp_tcp_endpoint_t *self = *self_p;
        memb_free(&zmtp_tcp_endpoints, self);
        *self_p = NULL;
    }
}

zmtp_connection_t *
zmtp_tcp_endpoint_connect (zmtp_tcp_endpoint_t *self)
{
    zmtp_connection_t *conn = memb_alloc(&zmtp_connections);
    if (conn == NULL) {
        printf("Reached exhaustion of zmtp_connections\r\n");
        return NULL;
    }

    PSOCK_INIT(&conn->ps, conn->buffer, sizeof(conn->buffer));
    conn->ip_conn = tcp_connect(&self->addr, self->port, conn);

    return conn;
}

zmtp_connection_t *
zmtp_tcp_endpoint_listen (zmtp_tcp_endpoint_t *self)
{
    zmtp_connection_t *conn = memb_alloc(&zmtp_connections);
    if (conn == NULL) {
        printf("Reached exhaustion of zmtp_connections\r\n");
        return NULL;
    }

    PSOCK_INIT(&conn->ps, conn->buffer, sizeof(conn->buffer));

    tcp_listen(self->port);

    return conn;
}

/*-----------------------------------------------------------*/

enum {
    ZMTP_MSG_MORE = 1,
    ZMTP_MSG_COMMAND = 4,
};

#ifndef ZMTP_MAX_MSGS
  #define ZMTP_MAX_MSGS 4
#endif

struct _zmtp_msg_t {
    uint8_t flags;                 //  Flags byte for message
    uint8_t *data;                 //  Data part of message
    size_t size;                //  Size of data in bytes
    uint8_t greedy;                //  Did we take ownership of data?
};
typedef struct _zmtp_msg_t zmtp_msg_t;
MEMB(zmtp_messages, zmtp_msg_t, ZMTP_MAX_MSGS);

//  Constructor; it allocates buffer for message data.
//  The initial content of the allocated buffer is undefined.

zmtp_msg_t *
zmtp_msg_new (uint8_t flags, size_t size)
{
    zmtp_msg_t *self = memb_alloc(&zmtp_messages);
    if(self == NULL) {
      printf("Reached exhaustion of messages\r\n");
      return NULL;
    }

    self->flags = flags;
    self->data = (uint8_t *) malloc (size);
    self->size = size;
    self->greedy = 1;
    return self;
}

//  Constructor; takes ownership of data and frees it when destroying the
//  message. Nullifies the data reference.

zmtp_msg_t *
zmtp_msg_from_data (uint8_t flags, uint8_t **data_p, size_t size)
{
    zmtp_msg_t *self = memb_alloc(&zmtp_messages);
    if(self == NULL) {
      printf("Reached exhaustion of messages\r\n");
      return NULL;
    }

    self->flags = flags;
    self->data = *data_p;
    self->size = size;
    self->greedy = 1;
    *data_p = NULL;
    return self;
}

//  Constructor that takes a constant data and does not copy, modify, or
//  free it.

zmtp_msg_t *
zmtp_msg_from_const_data (uint8_t flags, void *data, size_t size)
{
    zmtp_msg_t *self = memb_alloc(&zmtp_messages);
    if(self == NULL) {
      printf("Reached exhaustion of messages\r\n");
      return NULL;
    }

    self->flags = flags;
    self->data = data;
    self->size = size;
    self->greedy = 0;
    return self;
}

void
zmtp_msg_destroy (zmtp_msg_t **self_p)
{
    if (*self_p) {
        zmtp_msg_t *self = *self_p;
        if (self->greedy)
            free (self->data);
        memb_free (&zmtp_messages, self);
        *self_p = NULL;
    }
}

uint8_t
zmtp_msg_flags (zmtp_msg_t *self)
{
    return self->flags;
}

uint8_t *
zmtp_msg_data (zmtp_msg_t *self)
{
    return self->data;
}

size_t
zmtp_msg_size (zmtp_msg_t *self)
{
    return self->size;
}

/*-----------------------------------------------------------*/

enum {
    ZMTP_MORE_FLAG = 1,
    ZMTP_LARGE_FLAG = 2,
    ZMTP_COMMAND_FLAG = 4,
};

struct zmtp_greeting {
    uint8_t signature [10];
    uint8_t version [2];
    uint8_t mechanism [20];
    uint8_t as_server [1];
    uint8_t filler [31];
};

#ifndef ZMTP_MAX_CHANNELS
  #define ZMTP_MAX_CHANNELS 10
#endif

struct _zmtp_channel_t {
    zmtp_connection_t *conn;
};
typedef struct _zmtp_channel_t zmtp_channel_t;
MEMB(zmtp_channels, zmtp_channel_t, ZMTP_MAX_CHANNELS);

zmtp_channel_t *
zmtp_channel_new ()
{
    zmtp_channel_t *self = memb_alloc(&zmtp_channels);
    if(self == NULL) {
      printf("Reached exhaustion of channels\r\n");
      return NULL;
    }

    self->conn = NULL;

    return self;
}

void
zmtp_channel_destroy (zmtp_channel_t **self_p)
{
    if (*self_p) {
        zmtp_channel_t *self = *self_p;
        if(self->conn != NULL) {
          PSOCK_CLOSE(&self->conn->ps);
          memb_free(&zmtp_connections, self->conn);
        }
        memb_free(&zmtp_channels, self);
        *self_p = NULL;
    }
}

static int
s_negotiate (zmtp_channel_t *self);

int
zmtp_channel_tcp_connect (zmtp_channel_t *self,
                          uip_ipaddr_t *addr, unsigned short port)
{
    if (self->conn != NULL)
        return -1;

    zmtp_tcp_endpoint_t *endpoint = zmtp_tcp_endpoint_new (addr, port);
    if (endpoint == NULL)
        return -1;

    self->conn = zmtp_tcp_endpoint_connect (endpoint);
    zmtp_tcp_endpoint_destroy (&endpoint);
    if (self->conn == NULL)
        return -1;

    return 0;
}

int
zmtp_channel_tcp_listen (zmtp_channel_t *self, unsigned short port)
{
    if (self->conn != NULL)
        return -1;

    zmtp_tcp_endpoint_t *endpoint = zmtp_tcp_endpoint_new (NULL, port);

    self->conn = zmtp_tcp_endpoint_listen (endpoint);
    zmtp_tcp_endpoint_destroy (&endpoint);
    if (self->conn == NULL)
        return -1;

    return 0;
}

int zmtp_channel_send (zmtp_channel_t *self, zmtp_msg_t *msg);
zmtp_msg_t *zmtp_channel_recv (zmtp_channel_t *self);

static
PT_THREAD(s_tcp_send (zmtp_connection_t *conn, const void *data, size_t len))
{
    PSOCK_BEGIN(&conn->ps);

    printf("Sending (%d): ", len);
    print_data(data, len);
    printf("\r\n");

    PSOCK_SEND(&conn->ps, (uint8_t *) data, len);

    printf("Sent (%d): ", len);
    print_data(data, len);
    printf("\r\n");

    PSOCK_END(&conn->ps);
}

static
PT_THREAD(s_tcp_recv (zmtp_connection_t *conn, void *buffer, size_t len))
{
    PSOCK_BEGIN(&conn->ps);
    // uint8_t *original_buffer = conn->ps.bufptr;
    // conn->ps.bufptr = buffer;
    PSOCK_READBUF_LEN(&conn->ps, len);
    memcpy(buffer, conn->buffer, len);
    // conn->ps.bufptr = original_buffer;

    printf("Read (%d): ", len);
    print_data(buffer, len);
    printf("\r\n");

    PSOCK_END(&conn->ps);
}

uint8_t outgoing_sign[10] = { 0xff, 0, 0, 0, 0, 0, 0, 0, 1, 0x7f };

PT_THREAD(handle_connection(zmtp_channel_t *self)) {
    PT_BEGIN(&hdl_conn_pt);
    // PSOCK_BEGIN(&self->conn->ps);

    /*if (s_negotiate (self) == -1) {
        PSOCK_CLOSE(&self->conn->ps);
        memb_free(&zmtp_connections, self->conn);
        self->conn = NULL;
    }*/

    printf("Trying to send\r\n");

    PT_WAIT_THREAD(&hdl_conn_pt, s_tcp_recv (self->conn, outgoing_sign, 1));
    printf("Got %02hhX\r\n", *outgoing_sign);

    // PT_WAIT_THREAD(&hdl_conn_pt, s_tcp_recv (self->conn, outgoing_sign, 1));
    // printf("Got %02hhX\r\n", *outgoing_sign);
    //
    // PT_WAIT_THREAD(&hdl_conn_pt, s_tcp_recv (self->conn, outgoing_sign, 1));
    // printf("Got %02hhX\r\n", *outgoing_sign);

    PT_WAIT_THREAD(&hdl_conn_pt, s_tcp_send (self->conn, outgoing_sign, sizeof outgoing_sign));
    // s_tcp_send (self->conn, outgoing_sign, sizeof outgoing_sign);

    // size_t len = 10;
    // void *data = outgoing.signature;
    //
    // printf("Sending (%d): ", len);
    // print_data(data, len);
    // printf("\r\n");
    //
    // PSOCK_SEND(&self->conn->ps, (uint8_t *) data, len);
    //
    // printf("Sent (%d): ", len);
    // print_data(data, len);
    // printf("\r\n");

    PT_WAIT_THREAD(&hdl_conn_pt, s_tcp_recv (self->conn, outgoing_sign, 1));
    printf("Got %02hhX\r\n", *outgoing_sign);

    printf("Done\r\n");

    PT_END(&hdl_conn_pt);
    // PSOCK_END(&self->conn->ps);
}

static int
s_negotiate (zmtp_channel_t *self)
{
    if(self->conn == NULL)
        goto io_error;

    printf("Negociating\r\n");

    zmtp_connection_t *s = self->conn;

    //  This is our greeting (64 octets)
    const struct zmtp_greeting outgoing = {
        .signature = { 0xff, 0, 0, 0, 0, 0, 0, 0, 1, 0x7f },
        .version   = { 3, 0 },
        .mechanism = { 'N', 'U', 'L', 'L', '\0' }
    };
    //  Send protocol signature
    if (s_tcp_send (s, outgoing.signature, sizeof outgoing.signature) == -1)
        goto io_error;

    printf("Sent signature\r\n");

    //  Read the first byte.
    struct zmtp_greeting incoming;
    if (s_tcp_recv (s, incoming.signature, 1) == -1)
        goto io_error;
    if(incoming.signature [0] != 0xff)
        goto io_error;

    //  Read the rest of signature
    if (s_tcp_recv (s, incoming.signature + 1, 9) == -1)
        goto io_error;
    if((incoming.signature [9] & 1) == 1)
        goto io_error;

    printf("Verified signature\r\n");

    //  Exchange major version numbers
    if (s_tcp_send (s, outgoing.version, 1) == -1)
        goto io_error;
    if (s_tcp_recv (s, incoming.version, 1) == -1)
        goto io_error;

    if(incoming.version [0] == 3)
        goto io_error;

    printf("Remote is version 3\r\n");

    //  Send the rest of greeting to the peer.
    if (s_tcp_send (s, outgoing.version + 1, 1) == -1)
        goto io_error;
    if (s_tcp_send (s, outgoing.mechanism, sizeof outgoing.mechanism) == -1)
        goto io_error;
    if (s_tcp_send (s, outgoing.as_server, sizeof outgoing.as_server) == -1)
        goto io_error;
    if (s_tcp_send (s, outgoing.filler, sizeof outgoing.filler) == -1)
        goto io_error;

    printf("Sent rest of the greeting\r\n");

    //  Receive the rest of greeting from the peer.
    if (s_tcp_recv (s, incoming.version + 1, 1) == -1)
        goto io_error;
    if (s_tcp_recv (s, incoming.mechanism, sizeof incoming.mechanism) == -1)
        goto io_error;
    if (s_tcp_recv (s, incoming.as_server, sizeof incoming.as_server) == -1)
        goto io_error;
    if (s_tcp_recv (s, incoming.filler, sizeof incoming.filler) == -1)
        goto io_error;

    printf("Received rest of the greeting\r\n");

    //  Send READY command
    zmtp_msg_t *ready = zmtp_msg_from_const_data (0x04, "\5READY", 6);
    if(ready == NULL)
        goto io_error;
    zmtp_channel_send (self, ready);
    zmtp_msg_destroy (&ready);

    printf("Sent READY\r\n");

    //  Receive READY command
    ready = zmtp_channel_recv (self);
    if (!ready)
        goto io_error;
    if((zmtp_msg_flags (ready) & ZMTP_MSG_COMMAND) == ZMTP_MSG_COMMAND)
        goto io_error;
    zmtp_msg_destroy (&ready);

    printf("Received READY\r\n");

    return 0;

io_error:
    return -1;
}

int
zmtp_channel_send (zmtp_channel_t *self, zmtp_msg_t *msg)
{
    uint8_t frame_flags = 0;
    if ((zmtp_msg_flags (msg) & ZMTP_MSG_MORE) == ZMTP_MSG_MORE)
        frame_flags |= ZMTP_MORE_FLAG;
    if ((zmtp_msg_flags (msg) & ZMTP_MSG_COMMAND) == ZMTP_MSG_COMMAND)
        frame_flags |= ZMTP_COMMAND_FLAG;
    if (zmtp_msg_size (msg) > 255)
        frame_flags |= ZMTP_LARGE_FLAG;
    if (s_tcp_send (self->conn, &frame_flags, sizeof frame_flags) == -1)
        return -1;

    if (zmtp_msg_size (msg) <= 255) {
        const uint8_t msg_size = zmtp_msg_size (msg);
        if (s_tcp_send (self->conn, &msg_size, sizeof msg_size) == -1)
            return -1;
    }
    else {
        uint8_t buffer [8];
        const uint64_t msg_size = (uint64_t) zmtp_msg_size (msg);
        buffer [0] = msg_size >> 56;
        buffer [1] = msg_size >> 48;
        buffer [2] = msg_size >> 40;
        buffer [3] = msg_size >> 32;
        buffer [4] = msg_size >> 24;
        buffer [5] = msg_size >> 16;
        buffer [6] = msg_size >> 8;
        buffer [7] = msg_size;
        if (s_tcp_send (self->conn, buffer, sizeof buffer) == -1)
            return -1;
    }
    if (s_tcp_send (self->conn, zmtp_msg_data (msg), zmtp_msg_size (msg)) == -1)
        return -1;
    return 0;
}

zmtp_msg_t *
zmtp_channel_recv (zmtp_channel_t *self)
{
    uint8_t frame_flags;
    size_t size;

    if (s_tcp_recv (self->conn, &frame_flags, 1) == -1)
        return NULL;
    //  Check large flag
    if ((frame_flags & ZMTP_LARGE_FLAG) == 0) {
        uint8_t buffer [1];
        if (s_tcp_recv (self->conn, buffer, 1) == -1)
            return NULL;
        size = (size_t) buffer [0];
    }
    else {
        uint8_t buffer [8];
        if (s_tcp_recv (self->conn, buffer, sizeof buffer) == -1)
            return NULL;
        size = (uint64_t) buffer [0] << 56 |
               (uint64_t) buffer [1] << 48 |
               (uint64_t) buffer [2] << 40 |
               (uint64_t) buffer [3] << 32 |
               (uint64_t) buffer [4] << 24 |
               (uint64_t) buffer [5] << 16 |
               (uint64_t) buffer [6] << 8  |
               (uint64_t) buffer [7];
    }
    uint8_t *data = malloc (size);
    if(data == NULL)
        return NULL;
    if (s_tcp_recv (self->conn, data, size) == -1) {
        free (data);
        return NULL;
    }
    uint8_t msg_flags = 0;
    if ((frame_flags & ZMTP_MORE_FLAG) == ZMTP_MORE_FLAG)
        msg_flags |= ZMTP_MSG_MORE;
    if ((frame_flags & ZMTP_COMMAND_FLAG) == ZMTP_COMMAND_FLAG)
        msg_flags |= ZMTP_MSG_COMMAND;
    return zmtp_msg_from_data (msg_flags, &data, size);
}

void init() {
  memb_init(&zmtp_tcp_endpoints);
  memb_init(&zmtp_connections);
  memb_init(&zmtp_messages);
  memb_init(&zmtp_channels);

  PT_INIT(&hdl_conn_pt);
}

/********/
PROCESS(tcp_server_process, "TCP echo process");
AUTOSTART_PROCESSES(&tcp_server_process);
/*---------------------------------------------------------------------------*/
static void print_data(const uint8_t *data, int data_size) {
  int i;
  for(i=0 ; i < data_size ; i++)
    printf("%02hhX ", data[i]);
}
/*---------------------------------------------------------------------------*/

static struct psock ps;
static uint8_t buffer[100];

PT_THREAD(test(struct psock *p)) {//}, const void *data, size_t len)) {

uint8_t outgoing_signn[10] = { 0xff, 0, 0, 0, 0, 0, 0, 0, 1, 0x7f };
size_t len = sizeof outgoing_signn;
void *data = outgoing_signn;

    PSOCK_BEGIN(p);

    printf("Trying to send\r\n");

    //PT_WAIT_THREAD(&hdl_conn_pt, s_tcp_recv (self->conn, outgoing.signature, sizeof outgoing.signature));
    // s_tcp_recv (self->conn, outgoing.signature, sizeof outgoing.signature);

    printf("Sending (%d): ", len);
    print_data(data, len);
    printf("\r\n");

    PSOCK_SEND(p, (uint8_t *) data, len);

    printf("Sent (%d): ", len);
    print_data(data, len);
    printf("\r\n");

    printf("Done\r\n");
    PSOCK_END(p);
}

PT_THREAD(test2(struct psock *p)) {
    PT_BEGIN(&hdl_conn_pt);
    printf("Calling\r\n");
    PT_WAIT_THREAD(&hdl_conn_pt, test(p));//, outgoing_sign, sizeof outgoing_sign));
    // test(p);
    printf("Called\r\n");
    PT_END(&hdl_conn_pt);
}

zmtp_channel_t *chan = NULL;

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(tcp_server_process, ev, data)
{
  PROCESS_BEGIN();

  init();
  chan = zmtp_channel_new();
  printf("Channel: %p\r\n", chan);
  int rc = zmtp_channel_tcp_listen(chan, SERVER_PORT);
  printf("listen on %d: %d\r\n", SERVER_PORT, rc);

  PSOCK_INIT(&ps, buffer, sizeof(buffer));
  printf("Initialised psock\r\n");
  //
  // tcp_listen(UIP_HTONS(SERVER_PORT));
  // printf("listening on 9999");


  while(1) {
      PROCESS_WAIT_EVENT_UNTIL(ev == tcpip_event);
      printf("Got event %d, %d\r\n", ev, uip_flags);
      if(uip_connected()) {
          printf("Connected\r\n");
          while(!(uip_aborted() || uip_closed() || uip_timedout())) {
              PROCESS_WAIT_EVENT_UNTIL(ev == tcpip_event);
              printf("Got event ! %d, %d\r\n", ev, uip_flags);
              // test2(&ps);
              handle_connection(chan);
          }
      }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
