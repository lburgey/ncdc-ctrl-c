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
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef HAVE_LINUX_SENDFILE
# include <sys/sendfile.h>
#elif HAVE_BSD_SENDFILE
# include <sys/socket.h>
# include <sys/uio.h>
#endif


// global network stats
struct ratecalc net_in, net_out;

// Network states
#define NETST_IDL 0 // idle, disconnected
#define NETST_DNS 1 // resolving DNS
#define NETST_CON 2 // connecting
#define NETST_ASY 3 // connected, handling async messages
#define NETST_SYN 4 // connected, handling a synchronous send/receive
#define NETST_DIS 5 // disconnecting (cleanly)

#if INTERFACE

// actions that can fail
#define NETERR_CONN 0
#define NETERR_RECV 1
#define NETERR_SEND 2

struct dnscon;

struct net {
  int state;

  struct ratecalc *rate_in;
  struct ratecalc *rate_out;

  struct dnscon *dnscon; // state DNS,CON. Setting ->net to NULL 'cancels' DNS resolving.
  int sock; // state CON,ASY,SYN,DIS
  int socksrc; // state CON,ASY,DIS. Glib event source on 'sock'.

  // Called when an error has occured. Second argument is NETERR_*, third a
  // string representing the error. The net struct is always in NETST_IDL after
  // this has been called.
  void (*cb_err)(struct net *, int, const char *);

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
#define net_sendraw(a,b,d)
#define net_send(a,b)
#define net_sendf(a,b,...)
#define net_sendfile(a,b,c,d,e,f)

#endif





// DNS resolution and connecting

struct dnscon {
  struct net *net;
  char *addr;
  int port;
  struct addrinfo *nfo;
  struct addrinfo *next;
  struct sockaddr_in laddr;
  char *err;
  void(*cb)(struct net *, const char *);
};


static GThreadPool *dns_pool = NULL;


static void dnscon_free(struct dnscon *r) {
  g_free(r->err);
  g_free(r->addr);
  freeaddrinfo(r->nfo);
  g_slice_free(struct dnscon, r);
}


static void dnscon_tryconn(struct net *n);

static void dnsconn_handleconn(struct net *n, int err) {
  // Successful.
  if(err == 0) {
    if(n->dnscon->cb)
      n->dnscon->cb(n, NULL);
    dnscon_free(n->dnscon);
    n->dnscon = NULL;
    // TODO: setup ASY state etc.
    return;
  }

  close(n->sock);
  n->sock = 0;

  // Error, but we've got more addresses to try!
  if(n->dnscon->next->ai_next) {
    n->dnscon->next = n->dnscon->next->ai_next;
    dnscon_tryconn(n);
    return;
  }

  // Error on the last try, time to give up.
  if(n->cb_err)
    n->cb_err(n, NETERR_CONN, g_strerror(err));
  net_disconnect(n);
}


static gboolean dnscon_conresult(gpointer dat) {
  struct net *n = dat;
  n->socksrc = 0;

  int err = 0;
  socklen_t len = sizeof(err);
  getsockopt(n->sock, SOL_SOCKET, SO_ERROR, &err, &len);
  dnsconn_handleconn(n, err);

  return FALSE;
}


static void dnscon_tryconn(struct net *n) {
  struct addrinfo *c = n->dnscon->next;
  // We can't handle IPv6 yet.
  g_return_if_fail(c->ai_family == AF_INET && c->ai_addrlen == sizeof(struct sockaddr_in));

  if(n->dnscon->cb) {
    struct sockaddr_in *sa = (struct sockaddr_in *)c->ai_addr;
    char a[100];
    g_snprintf(a, 100, "%s:%d", inet_ntoa(sa->sin_addr), ntohs(sa->sin_port));
    n->dnscon->cb(n, a);
  }

  // Create new socket and connect.
  n->sock = socket(AF_INET, SOCK_STREAM, 0);
  fcntl(n->sock, F_SETFL, fcntl(n->sock, F_GETFL, 0)|O_NONBLOCK);

  if(bind(n->sock, (struct sockaddr *)&n->dnscon->laddr, sizeof(n->dnscon->laddr)) < 0) {
    if(n->cb_err) {
      char *e = g_strdup_printf("Can't bind to local address: %s", g_strerror(errno));
      n->cb_err(n, NETERR_CONN, e);
      g_free(e);
    }
    net_disconnect(n);
    return;
  }
  int r = connect(n->sock, n->dnscon->next->ai_addr, n->dnscon->next->ai_addrlen);

  // The common case, I guess
  if(r && errno == EINPROGRESS) {
    GSource *src = fdsrc_new(n->sock, TRUE);
    g_source_set_callback(src, dnscon_conresult, n, NULL);
    n->socksrc = g_source_attach(src, NULL);
    g_source_unref(src);
    return;
  }

  dnsconn_handleconn(n, r == 0 ? 0 : errno);
}


// Called as an idle function from the dnscon_thread.
static gboolean dnscon_gotdns(gpointer dat) {
  struct dnscon *r = dat;
  struct net *n = r->net;
  // It's possible that a net_disconnect() has happened in the mean time. Free
  // and ignore the results in that case.
  if(!n) {
    dnscon_free(r);
    return FALSE;
  }

  // Handle error
  if(r->err) {
    if(n->cb_err)
      n->cb_err(n, NETERR_CONN, r->err);
    net_disconnect(n);
    return FALSE;
  }

  // Is it possible for getaddrinfo() to return an empty result set without an error?
  g_return_val_if_fail(r->nfo, FALSE);

  // Try connecting to each of the addresses.
  n->state = NETST_CON;
  r->next = r->nfo;
  dnscon_tryconn(n);
  return FALSE;
}


// Async DNS resolution in a background thread
static void dnscon_thread(gpointer dat, gpointer udat) {
  struct dnscon *r = dat;
  struct addrinfo hint;
  hint.ai_family = AF_INET;
  hint.ai_socktype = SOCK_STREAM;
  hint.ai_protocol = 0;
  hint.ai_flags = 0;
  char port[20];
  g_snprintf(port, sizeof(port), "%d", r->port);
  int n = getaddrinfo(r->addr, port, &hint, &r->nfo);
  if(n)
    r->err = g_strdup(n == EAI_SYSTEM ? g_strerror(errno) : gai_strerror(n));
  g_idle_add(dnscon_gotdns, r);
}






// Connection management

struct net *net_new(void *handle) {
  struct net *n = g_new0(struct net, 1);
  n->ref = 1;
  n->handle = handle;
  n->rate_in = g_slice_new0(struct ratecalc);
  n->rate_out = g_slice_new0(struct ratecalc);
  ratecalc_init(n->rate_in);
  ratecalc_init(n->rate_out);
  return n;
}


// *addr must be a string in the form of 'host:port', where ':port' is
// optional. The callback is called with an address each time a connection
// attempt is made. It is called with NULL when the connection was successful
// (at which point net_remoteaddr() should work).
// TODO: Rename to net_connect() once the old crap has been rewritten.
void net_connect2(struct net *n, const char *addr, int defport, const char *laddr, void(*cb)(struct net *, const char *)) {
  g_return_if_fail(n->state == NETST_IDL);

  struct dnscon *r = g_slice_new0(struct dnscon);
  r->addr = str_portsplit(addr, defport, &r->port);
  r->net = n;
  r->cb = cb;

  r->laddr.sin_family = AF_INET;
  r->laddr.sin_port = 0;
  if(laddr)
    inet_aton(laddr, &r->laddr.sin_addr);
  else
    r->laddr.sin_addr.s_addr = INADDR_ANY;

  n->dnscon = r;
  n->state = NETST_DNS;
  g_thread_pool_push(dns_pool, r, NULL);
}


void net_disconnect(struct net *n) {
  switch(n->state) {

  case NETST_DNS:
    n->dnscon->net = NULL;
    n->dnscon = NULL;
    break;

  case NETST_CON:
    dnscon_free(n->dnscon);
    n->dnscon = NULL;
    if(n->sock)
      close(n->sock);
    if(n->socksrc)
      g_source_remove(n->socksrc);
    break;
  }
  n->state = NETST_IDL;
}


void net_unref(struct net *n) {
  if(!g_atomic_int_dec_and_test(&n->ref))
    return;
  g_return_if_fail(n->state == NETST_IDL);
  g_slice_free(struct ratecalc, n->rate_in);
  g_slice_free(struct ratecalc, n->rate_out);
  g_free(n);
}





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
  int port;
  char *ip = str_portsplit(dest, 412, &port);

  struct net_udp *m = g_slice_new0(struct net_udp);
  m->msg = g_strdup(msg);
  m->msglen = len;
  m->addr.sin_family = AF_INET;
  m->addr.sin_port = htons(port);
  inet_aton(ip, &m->addr.sin_addr);
  g_free(ip);

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

  dns_pool = g_thread_pool_new(dnscon_thread, NULL, -1, FALSE, NULL);

  // TODO: IPv6?
  net_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
  fcntl(net_udp_sock, F_SETFL, fcntl(net_udp_sock, F_GETFL, 0)|O_NONBLOCK);
  net_udp_queue = g_queue_new();
}

