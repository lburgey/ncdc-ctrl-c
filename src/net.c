/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011-2012 Yoran Heling

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/


#include "ncdc.h"
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#ifdef HAVE_LINUX_SENDFILE
# include <sys/sendfile.h>
#elif HAVE_BSD_SENDFILE
# include <sys/socket.h>
# include <sys/uio.h>
#endif


// global network stats
struct ratecalc net_in, net_out;

#if INTERFACE

// actions that can fail
#define NETERR_CONN 0
#define NETERR_RECV 1
#define NETERR_SEND 2

struct net {
  struct ratecalc *rate_in;
  struct ratecalc *rate_out;

  // some pointer for use by the user
  void *handle;
  // reference counter
  int ref;

  // OLD STUFF
  int conn;
  int tls;
  gboolean connecting;
  unsigned short conn_defport;
#if TLS_SUPPORT
  gboolean (*conn_accept_cert)(GTlsConnection *, GTlsCertificate *, GTlsCertificateFlags, gpointer);
#endif
  char eom[2];
  void (*recv_msg_cb)(struct net *, char *);
  void (*recv_datain)(struct net *, char *data, int len);
  void (*file_cb)(struct net *);
  void (*cb_con)(struct net *);
  void (*cb_err)(struct net *, int, GError *);
  gboolean keepalive;
  time_t timeout_last;
};


#define net_ref(n) g_atomic_int_inc(&((n)->ref))

// OLD STUFF
#define net_remoteaddr(n) ""
#define net_file_left(n) 0
#define net_recv_left(n) 0
#define net_recvraw(a,b,c,d,e)
#define net_create(a,b,c,d,e) NULL
#define net_setconn(a,b,c,d)
#define net_connect(a,b,c,d,e,f)
#define net_disconnect(a)
#define net_unref(a)
#define net_sendraw(a,b,d)
#define net_send(a,b)
#define net_sendf(a,b,...)
#define net_sendfile(a,b,c,d,e,f)

#endif






// Some global stuff for sending UDP packets

struct net_udp { struct sockaddr_in addr; char *msg; int msglen; };
static int net_udp_sock;
static GQueue *net_udp_queue;


static gboolean udp_handle_out(gpointer dat) {
  struct net_udp *m = g_queue_pop_head(net_udp_queue);
  if(!m)
    return FALSE;

  int n = sendto(net_udp_sock, m->msg, m->msglen, 0, (struct sockaddr *)&m->addr, sizeof(m->addr));
  // TODO: handle EWOULDBLOCK / EAGAIN / EINTR here?

  if(n != m->msglen)
    g_message("Error sending UDP message: %s.", g_strerror(errno));
  else {
    ratecalc_add(&net_out, m->msglen);
    g_debug("UDP:%s:%d> %s", inet_ntoa(m->addr.sin_addr), ntohs(m->addr.sin_port), m->msg);
  }
  g_free(m->msg);
  g_slice_free(struct net_udp, m);
  return net_udp_queue->head ? TRUE : FALSE;
}


// dest is assumed to be a valid IPv4 address with an optional port ("x.x.x.x" or "x.x.x.x:p")
void net_udp_send_raw(const char *dest, const char *msg, int len) {
  char *destc = g_strdup(dest);
  char *port_str = strchr(destc, ':');
  int port = 412;
  if(port_str) {
    *port_str = 0;
    port_str++;
    port = strtol(port_str, NULL, 10);
    if(port < 0 || port > 0xFFFF) {
      g_free(destc);
      return;
    }
  }

  struct net_udp *m = g_slice_new0(struct net_udp);
  m->msg = g_strdup(msg);
  m->msglen = len;
  m->addr.sin_family = AF_INET;
  m->addr.sin_port = htons(port);
  inet_aton(destc, &m->addr.sin_addr);

  g_queue_push_tail(net_udp_queue, m);
  if(net_udp_queue->head == net_udp_queue->tail) {
    GSource *src = fdsrc_new(net_udp_sock, TRUE);
    g_source_set_callback(src, udp_handle_out, NULL, NULL);
    g_source_attach(src, NULL);
    g_source_unref(src);
  }
}


void net_udp_send(const char *dest, const char *msg) {
  net_udp_send_raw(dest, msg, strlen(msg));
}


void net_udp_sendf(const char *dest, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  char *str = g_strdup_vprintf(fmt, va);
  va_end(va);
  net_udp_send_raw(dest, str, strlen(str));
  g_free(str);
}






// initialize some global structures

void net_init_global() {
  ratecalc_init(&net_in);
  ratecalc_init(&net_out);
  // Don't group these with RCC_UP or RCC_DOWN, otherwise bandwidth will be counted twice.
  ratecalc_register(&net_in, RCC_NONE);
  ratecalc_register(&net_out, RCC_NONE);

  // TODO: IPv6?
  net_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
  fcntl(net_udp_sock, F_SETFL, fcntl(net_udp_sock, F_GETFL, 0)|O_NONBLOCK);
  net_udp_queue = g_queue_new();
}

