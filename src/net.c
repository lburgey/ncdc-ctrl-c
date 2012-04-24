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

#define NET_RECV_SIZE (8*1024)
#define NET_MAX_RBUF (1024*1024)

#if INTERFACE

// Network states
#define NETST_IDL 0 // idle, disconnected
#define NETST_DNS 1 // resolving DNS
#define NETST_CON 2 // connecting
#define NETST_ASY 3 // connected, handling async messages
#define NETST_SYN 4 // connected, handling a synchronous send/receive
#define NETST_DIS 5 // disconnecting (cleanly)

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
  char addr[64]; // state ASY,SYN,DIS

  gnutls_session_t tls; // state ASY,SYN,DIS (only if tls is enabled)
  gboolean tls_handshake; // state ASY, whether we're handshaking.

  GString *rbuf; // state ASY. Read buffer.
  GString *wbuf; // state ASY. Write buffer.
  gboolean writing; // state ASY. Whether 'socksrc' is write poll event.

  // Called when an error has occured. Second argument is NETERR_*, third a
  // string representing the error. The net struct is always in NETST_IDL after
  // this has been called.
  void (*cb_err)(struct net *, int, const char *);

  // Message reading. Callback will be called only once. (Both state ASY)
  void (*msg_read)(struct net *, char *);
  char msg_eom;

  // some pointer for use by the user
  void *handle;
  // reference counter
  int ref;

  // OLD STUFF
  int conn;
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

#define net_remoteaddr(n) ((n)->addr)

// OLD STUFF
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





// Asynchronous message handling

static void asy_setuppoll(struct net *n);


// Checks rbuf against any queued read events and handles those. (Can be called
// as a glib idle function)
static gboolean asy_handlerbuf(gpointer dat) {
  struct net *n = dat;
  // The callbacks itself may in turn call other net_* functions, and thus
  // immediately queue another read action. Hence the while loop. Note that no
  // net_* function that remains in the ASY state is allowed to modify rbuf,
  // otherwise we need to make a copy of rbuf before passing it to the
  // callback.
  net_ref(n);
  while(n->state == NETST_ASY && n->rbuf->len && n->msg_read) {
    char *eom = memchr(n->rbuf->str, (unsigned char)n->msg_eom, n->rbuf->len);
    if(!eom)
      break;
    void(*cb)(struct net *, char *) = n->msg_read;
    n->msg_read = NULL;
    *eom = 0;
    g_debug("%s< %s%c", net_remoteaddr(n), n->rbuf->str, n->msg_eom != '\n' ? n->msg_eom : ' ');
    cb(n, n->rbuf->str);
    if(n->state == NETST_ASY)
      g_string_erase(n->rbuf, 0, eom - n->rbuf->str + 1);
  }
  net_unref(n);
  return FALSE;
}


// Tries a read. Returns FALSE if there was an error other than "please try
// again later".
static gboolean asy_read(struct net *n) {
  // Make sure we have enough buffer space
  if(n->rbuf->allocated_len < NET_MAX_RBUF && n->rbuf->allocated_len - n->rbuf->len < NET_RECV_SIZE) {
    gsize oldlen = n->rbuf->len;
    g_string_set_size(n->rbuf, MIN(NET_MAX_RBUF, n->rbuf->len+NET_RECV_SIZE));
    n->rbuf->len = oldlen;
  }
  int len = n->rbuf->allocated_len - n->rbuf->len - 1;
  if(len <= 10) { // Some arbitrary low number.
    if(n->cb_err)
      n->cb_err(n, NETERR_RECV, "Read buffer full");
    net_disconnect(n);
    return FALSE;
  }

  int r = n->tls
    ? gnutls_record_recv(n->tls, n->rbuf->str + n->rbuf->len, len)
    : recv(n->sock,              n->rbuf->str + n->rbuf->len, len, 0);

  // No data? Just return.
  if(r < 0 && (n->tls ? !gnutls_error_is_fatal(r) : errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN))
    return TRUE;

  // Handle error and disconnect
  if(r <= 0) {
    if(n->cb_err) {
      char *e = g_strdup_printf("Read error: %s", !r || !n->tls ? g_strerror(!r ? ECONNRESET : errno) : gnutls_strerror(r));
      n->cb_err(n, NETERR_RECV, e);
      g_free(e);
    }
    net_disconnect(n);
    return FALSE;
  }

  // Otherwise, update buffer info
  g_return_val_if_fail(n->rbuf->len + r < n->rbuf->allocated_len, FALSE);
  n->rbuf->len += r;
  n->rbuf->str[n->rbuf->len] = 0;
  ratecalc_add(&net_in, r);
  ratecalc_add(n->rate_in, r);
  net_ref(n);
  asy_handlerbuf(n);
  gboolean ret = n->state == NETST_ASY;
  net_unref(n);
  return ret;
}


static gboolean asy_write(struct net *n) {
  if(!n->wbuf->len)
    return TRUE;

  int r = n->tls
    ? gnutls_record_send(n->tls, n->wbuf->str, n->wbuf->len)
    : send(n->sock,              n->wbuf->str, n->wbuf->len, 0);

  // No data? Just return. (Note: r == 0 is seen as a temporary error)
  if(!r || (r < 0 && (n->tls ? !gnutls_error_is_fatal(r) : errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)))
    return TRUE;

  // Handle error
  if(r < 0) {
    if(n->cb_err) {
      char *e = g_strdup_printf("Write error: %s", n->tls ? gnutls_strerror(errno) : g_strerror(errno));
      n->cb_err(n, NETERR_SEND, e);
      g_free(e);
    }
    net_disconnect(n);
    return FALSE;
  }

  g_string_erase(n->wbuf, 0, r);
  ratecalc_add(&net_out, r);
  ratecalc_add(n->rate_out, r);
  return TRUE;
}


static gboolean asy_handshake(struct net *n) {
  if(!n->tls_handshake)
    return TRUE;

  int r = gnutls_handshake(n->tls);

  if(!r) { // Successful handshake
    g_debug("%s: TLS Handshake successful.", net_remoteaddr(n));
    n->tls_handshake = FALSE;
  } else if(gnutls_error_is_fatal(r)) { // Error
    if(n->cb_err) {
      char *e = g_strdup_printf("TLS error: %s", gnutls_strerror(r));
      n->cb_err(n, NETERR_RECV, e);
      g_free(e);
    }
    net_disconnect(n);
    return FALSE;
  }
  return TRUE;
}


static gboolean asy_pollresult(gpointer dat) {
  struct net *n = dat;
  n->socksrc = 0;

  // Handshake
  if(n->tls_handshake && !asy_handshake(n))
    return FALSE;

  // Fill rbuf
  if(!n->tls_handshake && !asy_read(n))
    return FALSE;

  // Flush wbuf
  if(!n->tls_handshake && !asy_write(n))
    return FALSE;

  asy_setuppoll(n);
  return FALSE;
}


static void asy_setuppoll(struct net *n) {
  // If we already have the right poll source active, ignore this.
  gboolean wantwrite = n->tls ? gnutls_record_get_direction(n->tls) : !!n->wbuf->len;
  if(n->socksrc && (!wantwrite || n->writing))
    return;

  if(n->socksrc)
    g_source_remove(n->socksrc);

  n->writing = wantwrite;
  GSource *src = fdsrc_new(n->sock, G_IO_IN | (n->writing ? G_IO_OUT : 0));
  g_source_set_callback(src, asy_pollresult, n, NULL);
  n->socksrc = g_source_attach(src, NULL);
  g_source_unref(src);
}


// Will run the specified callback once a full message has been received. A
// "message" meaning any bytes before reading the EOM character. The EOM
// character is not passed to the callback.
// Only a single net_readmsg() can be queued at one time.
void net_readmsg(struct net *n, char eom, void(*cb)(struct net *, char *)) {
  g_return_if_fail(n->state == NETST_ASY);
  g_return_if_fail(!n->msg_read);
  n->msg_eom = eom;
  n->msg_read = cb;
  g_idle_add(asy_handlerbuf, n);
}


#define flush if(n->tls_handshake || asy_write(n)) asy_setuppoll(n)

// This is often used to write a raw byte strings, so is not logged for debugging.
void net_write(struct net *n, const char *buf, int len) {
  g_return_if_fail(n->state == NETST_ASY);
  g_string_append_len(n->wbuf, buf, len);
  flush;
}


void net_writestr(struct net *n, const char *msg) {
  g_return_if_fail(n->state == NETST_ASY);
  g_debug("%s> %s", net_remoteaddr(n), msg);
  g_string_append(n->wbuf, msg);
  flush;
}


void net_writef(struct net *n, const char *fmt, ...) {
  g_return_if_fail(n->state == NETST_ASY);
  int old = n->wbuf->len;
  va_list va;
  va_start(va, fmt);
  g_string_append_vprintf(n->wbuf, fmt, va);
  va_end(va);
  g_debug("%s> %s", net_remoteaddr(n), n->wbuf->str+old);
  flush;
}

#undef flush


// Enables TLS-mode and initates the handshake. May not be called when there's
// something in the read or write buffer.
// TODO: Probably want to allow something in the read buffer in the future.
// TODO: Callback for certificate checking.
void net_settls(struct net *n, gboolean serv) {
  g_return_if_fail(n->state == NETST_ASY);
  g_return_if_fail(!n->rbuf->len && !n->wbuf->len);
  g_return_if_fail(!n->tls);
  gnutls_init(&n->tls, serv ? GNUTLS_SERVER : GNUTLS_CLIENT);
  gnutls_credentials_set(n->tls, GNUTLS_CRD_CERTIFICATE, db_certificate);
  gnutls_priority_set_direct(n->tls, "NORMAL", NULL); // TODO: Make this configurable. No, really.
  // TODO: Set custom read/write functions, necessary for correct rate calculation.
  gnutls_transport_set_ptr(n->tls, (gnutls_transport_ptr_t)(long)n->sock);
  n->tls_handshake = TRUE;
  asy_handshake(n);
}


void net_connected(struct net *n, int sock, const char *addr) {
  g_return_if_fail(n->state == NETST_IDL || n->state == NETST_CON);
  g_debug("%s: Connected.", addr);
  n->state = NETST_ASY;
  n->sock = sock;
  strncpy(n->addr, addr, sizeof(n->addr));
  n->wbuf = g_string_sized_new(1024);
  n->rbuf = g_string_sized_new(1024);

  ratecalc_reset(n->rate_in);
  ratecalc_reset(n->rate_out);
  ratecalc_register(n->rate_in, RCC_DOWN);
  ratecalc_register(n->rate_out, RCC_UP);

  asy_setuppoll(n); // Always make sure we're polling for read, to catch an async disconnect.
}






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
    char a[100];
    struct sockaddr_in *sa = (struct sockaddr_in *)n->dnscon->next->ai_addr;
    g_snprintf(a, 100, "%s:%d", inet_ntoa(sa->sin_addr), ntohs(sa->sin_port));
    net_connected(n, n->sock, a);

    if(n->dnscon->cb)
      n->dnscon->cb(n, NULL);
    dnscon_free(n->dnscon);
    n->dnscon = NULL;
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
    GSource *src = fdsrc_new(n->sock, G_IO_OUT);
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
    break;

  case NETST_ASY:
    n->msg_read = NULL;
    g_string_free(n->rbuf, TRUE);
    g_string_free(n->wbuf, TRUE);
    n->rbuf = n->wbuf = NULL;
    break;
  }

  // This is a force disconnect, not a graceful shutdown.
  if(n->tls) {
    gnutls_deinit(n->tls);
    n->tls = NULL;
  }
  if(n->sock) {
    close(n->sock);
    n->sock = 0;
  }
  if(n->socksrc) {
    g_source_remove(n->socksrc);
    n->socksrc = 0;
  }

  ratecalc_unregister(n->rate_in);
  ratecalc_unregister(n->rate_out);

  if(n->state == NETST_ASY || n->state == NETST_SYN || n->state == NETST_DIS)
    g_debug("%s: Disconnected (forced).", net_remoteaddr(n));
  n->addr[0] = 0;
  n->tls_handshake = FALSE;
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
    GSource *src = fdsrc_new(net_udp_sock, G_IO_OUT);
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

