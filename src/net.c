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
#include "net.h"
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
ratecalc_t net_in, net_out;

#define NET_RECV_SIZE (   8*1024)
#define NET_MAX_RBUF  (1024*1024)
#define NET_TRANS_BUF (  32*1024)

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
#define NETERR_TIMEOUT 3


struct dnscon;
struct synfer;

struct net {
  int state;

  ratecalc_t *rate_in;
  ratecalc_t *rate_out;

  struct dnscon *dnscon; // state DNS,CON. Setting ->net to NULL 'cancels' DNS resolving.
  int sock; // state CON,ASY,SYN,DIS
  int socksrc; // state CON,ASY,DIS. Glib event source on 'sock'.
  char addr[64]; // state ASY,SYN,DIS

  gnutls_session_t tls; // state ASY,SYN,DIS (only if tls is enabled)
  void (*cb_handshake)(struct net *, const char *); // state ASY, called after complete handshake.
  void (*cb_shutdown)(struct net *); // state DIS, called after complete disconnect.

  gboolean tls_handshake : 8; // state ASY, whether we're handshaking.
  gboolean shutdown_closed : 8; // state DIS, whether shutdown() has been called on the socket.
  gboolean writing : 8; // state ASY. Whether 'socksrc' is write poll event.
  gboolean wantwrite : 8; // state ASY. Whether we want a write on sock.

  GString *tlsrbuf; // state ASY. Temporary buffer for data read before switching to TLS. (To be fed to GnuTLS)
  GString *rbuf; // state ASY. Read buffer.
  GString *wbuf; // state ASY. Write buffer.

  // Called when an error has occured. Second argument is NETERR_*, third a
  // string representing the error.
  void (*cb_err)(struct net *, int, const char *);

  // Read buffer handling. Callback will be called only once. (State ASY)
  void (*rd_cb)(struct net *, char *, int len);
  gboolean rd_msg : 1; // TRUE: message, rd_dat=EOM; FALSE=bytes, rd_dat=count
  gboolean rd_consume : 1;
  int rd_dat;

  // Synchronous file transfers (SYN state) When set in the ASY state, it means
  // that buffers should be flushed before switching to the SYN state.
  struct synfer *syn;

  // some pointer for use by the user
  void *handle;
  // reference counter
  int ref;

  // Timeout handling
  int timeout_src;
  time_t timeout_last;
  const char *timeout_msg;
};


#define net_ref(n) g_atomic_int_inc(&((n)->ref))

#define net_remoteaddr(n) ((n)->addr)

#define net_is_connected(n) ((n)->state == NETST_ASY || (n)->state == NETST_SYN)
#define net_is_connecting(n) ((n)->state == NETST_DNS || (n)->state == NETST_CON)
#define net_is_idle(n) ((n)->state == NETST_IDL)

#endif






// Low-level recv/send wrappers

static ssize_t tls_pull(gnutls_transport_ptr_t dat, void *buf, size_t len) {
  struct net *n = dat;

  // Special buffer to allow passing read data back to the GnuTLS stream.
  if(n->tlsrbuf) {
    memcpy(buf, n->tlsrbuf->str, MIN(len, n->tlsrbuf->len));
    if(len >= n->tlsrbuf->len) {
      g_string_free(n->tlsrbuf, TRUE);
      n->tlsrbuf = NULL;
    } else
      g_string_erase(n->tlsrbuf, 0, len);
    return len;
  }

  // Otherwise, get the data directly from the network.
  int r = recv(n->sock, buf, len, 0);
  if(r < 0)
    gnutls_transport_set_errno(n->tls, errno == EWOULDBLOCK ? EAGAIN : errno);
  else {
    ratecalc_add(&net_in, r);
    ratecalc_add(n->rate_in, r);
  }
  return r;
}

// Behaves similarly to a normal recv(), but writes a readable error message to
// *err. If the error is temporary (e.g. EAGAIN), returns -1 but with *err=NULL.
// Does not return 0, disconnect is considered a fatal error.
static int low_recv(struct net *n, char *buf, int len, const char **err) {
  int r = n->tls
    ? gnutls_record_recv(n->tls, buf, len)
    : recv(n->sock,              buf, len, 0);

  if(r < 0 && (n->tls ? !gnutls_error_is_fatal(r) : errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)) {
    *err = NULL;
    return -1;
  }

  if(n->state != NETST_DIS)
    time(&n->timeout_last);
  if(r <= 0) {
    *err = !r || !n->tls ? g_strerror(!r ? ECONNRESET : errno) : gnutls_strerror(r);
    return -1;
  }

  if(!n->tls) {
    ratecalc_add(&net_in, r);
    ratecalc_add(n->rate_in, r);
  }
  return r;
}


static ssize_t tls_push(gnutls_transport_ptr_t dat, const void *buf, size_t len) {
  struct net *n = dat;
  int r = send(n->sock, buf, len, 0);
  if(r < 0)
    gnutls_transport_set_errno(n->tls, errno == EWOULDBLOCK ? EAGAIN : errno);
  else {
    ratecalc_add(&net_out, r);
    ratecalc_add(n->rate_out, r);
  }
  return r;
}


// Same as low_recv(), but for send().
static int low_send(struct net *n, const char *buf, int len, const char **err) {
  int r = n->tls
    ? gnutls_record_send(n->tls, buf, len)
    : send(n->sock,              buf, len, 0);

  // Note: r == 0 is seen as a temporary error
  if(!r || (r < 0 && (n->tls ? !gnutls_error_is_fatal(r) : errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN))) {
    *err = NULL;
    return -1;
  }

  if(n->state != NETST_DIS)
    time(&n->timeout_last);
  if(r < 0) {
    *err = n->tls ? gnutls_strerror(errno) : g_strerror(errno);
    return -1;
  }

  if(!n->tls) {
    ratecalc_add(&net_out, r);
    ratecalc_add(n->rate_out, r);
  }
  return r;
}






// Synchronous file transfers

static void asy_setuppoll(struct net *n);

struct synfer {
  GStaticMutex lock; // protects net->sock and net->tls in the case of a disconnect.
  struct net *net;
  int left;
  int fd;     // for uploads
  int cancel; // set to 1 to cancel transfer
  int can[2]; // close() this pipe (can[1]) to cancel the transfer
  gboolean upl : 1; // whether this is an upload or download
  gboolean flush : 1; // for uploads
  char *err;
  void *ctx; // for downloads
  void (*cb_downdone)(struct net *, void *);
  gboolean (*cb_downdata)(void *, const char *, int);
  void (*cb_upldone)(struct net *);
};

static GThreadPool *syn_pool = NULL;


static void syn_new(struct net *n, gboolean upl, int len) {
  n->syn = g_slice_new0(struct synfer);
  n->syn->left = len;
  n->syn->net = n;
  n->syn->upl = upl;

  g_static_mutex_init(&n->syn->lock);
  if(pipe(n->syn->can) < 0) {
    g_critical("pipe() failed: %s", g_strerror(errno));
    g_return_if_reached();
  }
  net_ref(n);
}


static void syn_free(struct synfer *s) {
  net_unref(s->net);
  close(s->can[0]);
  if(s->fd)
    close(s->fd);
  if(s->cb_downdone)
    s->cb_downdone(NULL, s->ctx);
  g_free(s->err);
  g_slice_free(struct synfer, s);
}


static void syn_cancel(struct net *n) {
  n->syn->cancel = 1;
  close(n->syn->can[1]);
  n->syn = NULL;
}


// Called as an idle function from syn_thread
static gboolean syn_done(gpointer dat) {
  struct synfer *s = dat;
  struct net *n = s->net;

  // Cancelled
  if(s->cancel) {
    syn_free(s);
    return FALSE;
  }

  // Error
  if(s->err) {
    g_debug("%s: Syn: %s", net_remoteaddr(n), s->err);
    syn_cancel(n);
    n->cb_err(n, s->upl ? NETERR_SEND : NETERR_RECV, s->err);
    syn_free(s);
    return FALSE;
  }

  syn_cancel(n);
  n->state = NETST_ASY;
  n->wantwrite = FALSE;
  asy_setuppoll(n);
  if(s->cb_upldone)
    s->cb_upldone(n);
  if(s->cb_downdone) {
    s->cb_downdone(n, s->ctx);
    s->cb_downdone = NULL;
  }
  syn_free(s);
  return FALSE;
}


// Does two things: Waits some time to ensure that we are allowed to burst with
// the rate limiting thing, and then waits for the socket to become
// readable/writable. Returns the number of bytes that may be read/written on
// success, 0 if the operation has been cancelled.
static int syn_wait(struct synfer *s, int sock, gboolean write) {
  // Lock to get the socket fd
  g_static_mutex_lock(&s->lock);
  GPollFD fds[2] = {};
  fds[0].fd = s->can[0];
  fds[0].events = G_IO_IN;
  fds[1].fd = sock;
  fds[1].events = write ? G_IO_OUT : G_IO_IN;
  g_static_mutex_unlock(&s->lock);

  // Poll for burst
  int b = 0;
  int r = 0;
  while(r <= 0 && (b = ratecalc_burst(write ? s->net->rate_out : s->net->rate_in)) <= 0) {
    // Wake up 4 times per second. If the resource is CPU or HDD I/O
    // constrained, then this means that at most 1/4th of the possible usage
    // time is "thrown away". I don't expect this to be much of an issue,
    // however.
    r = g_poll(fds, 1, 250); // only poll for the cancel fd here.
    g_return_val_if_fail(r >= 0 || errno == EINTR, 0);
  }
  if(r)
    return 0;

  // Now poll for read/writability of the socket.
  do
    r = g_poll(fds, 2, -1);
  while(r < 0 && errno == EINTR);

  if(fds[0].revents)
    return 0;
  return b;
}


#ifdef HAVE_SENDFILE

static void syn_upload_sendfile(struct synfer *s, int sock, fadv_t *adv) {
  off_t off = lseek(s->fd, 0, SEEK_CUR);
  if(off == (off_t)-1) {
    s->err = g_strdup(g_strerror(errno));
    return;
  }

  while(s->left > 0 && !s->err && !s->cancel) {
    off_t oldoff = off;
    int b = syn_wait(s, sock, TRUE);
    if(b <= 0)
      return;

    // No need for a lock here, we're not using the TLS session and socket fd's
    // are thread-safe. To some extent at least.
#ifdef HAVE_LINUX_SENDFILE
    ssize_t r = sendfile(sock, s->fd, &off, MIN(b, s->left));
#elif HAVE_BSD_SENDFILE
    off_t len = 0;
    gint64 r = sendfile(s->fd, sock, off, (size_t)MIN(b, s->left), NULL, &len, 0);
    // a partial write results in an EAGAIN error on BSD, even though this isn't
    // really an error condition at all.
    if(r != -1 || (r == -1 && errno == EAGAIN))
      r = len;
#endif

    if(r >= 0) {
      if(s->flush)
        fadv_purge(adv, r);
      off = oldoff + r;
      g_atomic_int_add(&s->left, -r);
      // This bypasses the low_send() function, so manually add it to the
      // ratecalc thing and update timeout_last.
      ratecalc_add(&net_out, r);
      ratecalc_add(s->net->rate_out, r);
      g_static_mutex_lock(&s->lock);
      time(&s->net->timeout_last);
      g_static_mutex_unlock(&s->lock);
      continue;
    } else if(errno == EAGAIN || errno == EINTR) {
      continue;
    } else if(errno == ENOTSUP || errno == ENOSYS || errno == EINVAL) {
      g_message("sendfile() failed with `%s', using fallback.", g_strerror(errno));
      // Don't set s->err here, let the fallback handle the rest
      return;
    } else {
      if(errno != EPIPE && errno != ECONNRESET)
        g_critical("sendfile() returned an unknown error: %d (%s)", errno, g_strerror(errno));
      s->err = g_strdup(g_strerror(errno));
      return;
    }
  }
}

#endif


static void syn_upload_buf(struct synfer *s, int sock, fadv_t *adv) {
  char *buf = g_malloc(NET_TRANS_BUF);

  while(s->left > 0 && !s->err && !s->cancel) {
    int rd = read(s->fd, buf, MIN(NET_TRANS_BUF, s->left));
    if(rd <= 0) {
      s->err = g_strdup(g_strerror(errno));
      goto done;
    }
    if(s->flush)
      fadv_purge(adv, rd);

    char *p = buf;
    while(rd > 0) {
      int b = syn_wait(s, sock, TRUE);
      if(b <= 0)
        goto done;

      g_static_mutex_lock(&s->lock);
      const char *err = NULL;
      int wr = s->cancel || !s->net->sock ? 0 : low_send(s->net, p, MIN(rd, b), &err);
      g_static_mutex_unlock(&s->lock);

      if(!wr) // cancelled
        goto done;
      if(wr < 0 && !err) // would block
        continue;
      if(wr < 0) { // actual error
        s->err = g_strdup(err);
        goto done;
      }
      // successful write
      p += wr;
      g_atomic_int_add(&s->left, -wr);
      rd -= wr;
    }
  }

done:
  g_free(buf);
}


static void syn_download(struct synfer *s, int sock) {
  char *buf = g_malloc(NET_TRANS_BUF);

  while(s->left > 0 && !s->err && !s->cancel) {
    int b = syn_wait(s, sock, FALSE);
    if(b <= 0)
      break;

    g_static_mutex_lock(&s->lock);
    const char *err = NULL;
    int r = s->cancel || !s->net->sock ? 0 : low_recv(s->net, buf, MIN(NET_TRANS_BUF, s->left), &err);
    g_static_mutex_unlock(&s->lock);

    if(!r)
      break;
    if(r < 0 && !err)
      continue;
    if(r < 0) {
      s->err = g_strdup(err);
      break;
    }

    g_atomic_int_add(&s->left, -r);
    if(!s->cb_downdata(s->ctx, buf, r)) {
      s->err = g_strdup("Operation cancelled");
      break;
    }
  }

  g_free(buf);
}


static void syn_thread(gpointer dat, gpointer udat) {
  struct synfer *s = dat;

  // Make a copy of sock to make sure it doesn't disappear on us.
  // (Still need to obtain the lock to make use of it).
  g_static_mutex_lock(&s->lock);
  int sock = s->net->sock;
  gboolean tls = !!s->net->tls;
  g_static_mutex_unlock(&s->lock);

  if(sock && !s->cancel && s->upl) {
    fadv_t adv;
    if(s->flush)
      fadv_init(&adv, s->fd, lseek(s->fd, 0, SEEK_CUR), VAR_FFC_UPLOAD);

#ifdef HAVE_SENDFILE
    if(!tls && var_get_bool(0, VAR_sendfile))
      syn_upload_sendfile(s, sock, &adv);
#endif
    if(s->left > 0 && !s->err && !s->cancel)
      syn_upload_buf(s, sock, &adv);

    if(s->flush)
      fadv_close(&adv);
  }

  if(sock && !s->cancel && !s->upl)
    syn_download(s, sock);

  g_idle_add(syn_done, s);
}


static void syn_start(struct net *n) {
  n->state = NETST_SYN;
  // We're coming from the ASY state, so make sure to clean this up.
  if(n->socksrc) {
    g_source_remove(n->socksrc);
    n->socksrc = 0;
  }
  g_thread_pool_push(syn_pool, n->syn, NULL);
}


int net_left(struct net *n) {
  return n->syn ? g_atomic_int_get(&n->syn->left) : 0;
}







// Asynchronous TLS handshaking & message handling & disconnecting

static gboolean handle_timer(gpointer dat);

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
  while(n->state == NETST_ASY && n->rbuf->len && n->rd_cb && !n->syn) {
    gboolean msg = n->rd_msg;
    gboolean consume = n->rd_consume;
    int dat = n->rd_dat;
    void(*cb)(struct net *, char *, int) = n->rd_cb;

    char *end = msg
      ? memchr(n->rbuf->str, dat, n->rbuf->len)
      : n->rbuf->len >= dat ? n->rbuf->str + dat : NULL;
    if(!end)
      break;
    n->rd_cb = NULL;
    if(msg) {
      *end = 0;
      if(consume)
        g_debug("%s< %s%c", net_remoteaddr(n), n->rbuf->str, dat != '\n' ? dat : ' ');
    }
    cb(n, n->rbuf->str, end - n->rbuf->str);
    if(n->state == NETST_ASY || n->state == NETST_SYN || n->state == NETST_DIS) {
      if(consume)
        g_string_erase(n->rbuf, 0, end - n->rbuf->str + (msg ? 1 : 0));
      else if(msg)
        *end = dat;
    }
  }

  // Handle recvfile
  if(n->syn && n->state == NETST_ASY && !n->syn->upl) {
    struct synfer *s = n->syn;
    if(n->rbuf->len) {
      int w = MIN(n->rbuf->len, s->left);
      s->left -= w;
      s->cb_downdata(s->ctx, n->rbuf->str, w);
      g_string_erase(n->rbuf, 0, w);
    }
    if(s->left)
      syn_start(n);
    else {
      s->cb_downdone(n, s->ctx);
      syn_cancel(n);
      syn_free(s);
    }
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
    g_debug("%s: Read buffer full", net_remoteaddr(n));
    n->cb_err(n, NETERR_RECV, "Read buffer full");
    return FALSE;
  }

  const char *err = NULL;
  int r = low_recv(n, n->rbuf->str + n->rbuf->len, len, &err);
  if(r < 0 && !err)
    return TRUE;

  // Handle error and disconnect
  if(r < 0) {
    char *e = g_strdup_printf("Read error: %s", err);
    g_debug("%s: %s", net_remoteaddr(n), e);
    n->cb_err(n, NETERR_RECV, e);
    g_free(e);
    return FALSE;
  }

  // Otherwise, update buffer info
  g_return_val_if_fail(n->rbuf->len + r < n->rbuf->allocated_len, FALSE);
  n->rbuf->len += r;
  n->rbuf->str[n->rbuf->len] = 0;
  net_ref(n);
  asy_handlerbuf(n);
  gboolean ret = n->state == NETST_ASY;
  net_unref(n);
  return ret;
}


static gboolean dis_shutdown(struct net *n) {
  // Shutdown TLS
  if(n->tls) {
    int r = gnutls_bye(n->tls, GNUTLS_SHUT_RDWR);
    if(r == 0) {
      gnutls_deinit(n->tls);
      n->tls = NULL;
    } else if(r < 0 && !gnutls_error_is_fatal(r)) {
      if(gnutls_record_get_direction(n->tls))
        n->wantwrite = TRUE;
      return TRUE;
    } else {
      char *e = g_strdup_printf("Shutdown error: %s", gnutls_strerror(r));
      g_debug("%s: %s", net_remoteaddr(n), e);
      n->cb_err(n, NETERR_RECV, e);
      g_free(e);
      return FALSE;
    }
  }

  // Shutdown socket
  if(!n->tls && !n->shutdown_closed) {
    shutdown(n->sock, SHUT_WR);
    n->shutdown_closed = TRUE;
  }

  // Wait for ACK (discard anything we read)
  if(!n->tls) {
    char buf[10];
    int r = recv(n->sock, buf, sizeof(buf), 0);
    if(r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      return TRUE;
    if(r < 0) {
      char *e = g_strdup_printf("Read error: %s", g_strerror(errno));
      g_debug("%s: %s", net_remoteaddr(n), e);
      n->cb_err(n, NETERR_RECV, e);
      g_free(e);
      return FALSE;
    }
    if(r == 0) {
      if(n->cb_shutdown)
        n->cb_shutdown(n);
      net_disconnect(n); // still do a force disconnect to clean up stuff and go to IDLE state
    }
  }
  return FALSE;
}


static gboolean asy_write(struct net *n) {
  if(!n->wbuf->len)
    return TRUE;

  const char *err = NULL;
  int r = low_send(n, n->wbuf->str, n->wbuf->len, &err);
  if(r < 0 && !err) {
    n->wantwrite = TRUE;
    return TRUE;
  }

  // Handle error
  if(r < 0) {
    char *e = g_strdup_printf("Write error: %s", err);
    g_debug("%s: %s", net_remoteaddr(n), e);
    n->cb_err(n, NETERR_SEND, e);
    g_free(e);
    return FALSE;
  }

  g_string_erase(n->wbuf, 0, r);

  if(!n->wbuf->len) {
    if(n->syn && n->syn->upl) {
      syn_start(n);
      return FALSE;
    }
    if(n->state == NETST_DIS)
      return dis_shutdown(n);
  }

  return TRUE;
}


static gboolean asy_handshake(struct net *n) {
  if(!n->tls_handshake)
    return TRUE;

  int r = gnutls_handshake(n->tls);

  if(!r) { // Successful handshake
    unsigned int len;
    char kpr[32] = {};
    char kpf[53] = {};
    const gnutls_datum_t *certs = gnutls_certificate_get_peers(n->tls, &len);
    if(certs && len >= 1) {
      certificate_sha256(*certs, kpr);
      base32_encode_dat(kpr, kpf, 32);
    }
    g_debug("%s: TLS Handshake successful, KP=SHA256/%s", net_remoteaddr(n), kpf);
    n->tls_handshake = FALSE;
    gboolean ret = TRUE;
    if(n->cb_handshake) {
      net_ref(n);
      n->cb_handshake(n, *kpf ? kpr : NULL);
      n->cb_handshake = NULL;
      ret = n->state == NETST_ASY;
      net_unref(n);
    }
    if(ret && n->syn) {
      syn_start(n);
      return FALSE;
    }
    return ret;

  } else if(gnutls_error_is_fatal(r)) { // Error
    char *e = g_strdup_printf("TLS error: %s", gnutls_strerror(r));
    g_debug("%s: %s", net_remoteaddr(n), e);
    n->cb_err(n, NETERR_RECV, e);
    g_free(e);
    return FALSE;
  }

  if(gnutls_record_get_direction(n->tls))
    n->wantwrite = TRUE;

  return TRUE;
}


static gboolean asy_pollresult(gpointer dat) {
  struct net *n = dat;
  n->socksrc = 0;
  n->wantwrite = FALSE;

  // Shutdown
  if(n->state == NETST_DIS) {
    if(dis_shutdown(n))
      asy_setuppoll(n);
    return FALSE;
  }

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
  if(n->socksrc && (!n->wantwrite || n->writing))
    return;

  if(n->socksrc)
    g_source_remove(n->socksrc);

  n->writing = n->wantwrite;
  GSource *src = fdsrc_new(n->sock, G_IO_IN | (n->writing ? G_IO_OUT : 0));
  g_source_set_callback(src, asy_pollresult, n, NULL);
  n->socksrc = g_source_attach(src, NULL);
  g_source_unref(src);
}


static void asy_setupread(struct net *n, gboolean msg, gboolean consume, int dat, void(*cb)(struct net *, char *, int)) {
  g_return_if_fail(n->state == NETST_ASY);
  n->rd_msg = msg;
  n->rd_consume = consume;
  n->rd_dat = dat;
  n->rd_cb = cb;
  g_idle_add(asy_handlerbuf, n);
}


// Will run the specified callback once a full message has been received. A
// "message" meaning any bytes before reading the EOM character. The EOM
// character is not passed to the callback.
// Only a single net_(read|peek) may be active at a single time.
void net_readmsg(struct net *n, unsigned char eom, void(*cb)(struct net *, char *, int)) {
  asy_setupread(n, TRUE, TRUE, eom, cb);
}


void net_readbytes(struct net *n, int bytes, void(*cb)(struct net *, char *, int)) {
  asy_setupread(n, FALSE, TRUE, bytes, cb);
}


// Will run the specified callback once at least the specified number of bytes
// are in the buffer. The data will remain in the buffer after the callback has
// run.
void net_peekbytes(struct net *n, int bytes, void(*cb)(struct net *, char *, int)) {
  asy_setupread(n, FALSE, FALSE, bytes, cb);
}


// Similar to net_readbytes(), but will call the data() callback for every read
// from the network, this callback may be run from another thread. When done,
// the done() callback will be run in the main thread.
void net_recvfile(struct net *n, int len, gboolean(*data)(void *, const char *, int), void(*done)(struct net *, void *), void *ctx) {
  g_return_if_fail(n->state == NETST_ASY);
  syn_new(n, FALSE, len);
  n->syn->cb_downdata = data;
  n->syn->cb_downdone = done;
  n->syn->ctx = ctx;
  n->rd_cb = NULL;
  g_idle_add(asy_handlerbuf, n);
}


#define flush if(n->tls_handshake || asy_write(n)) asy_setuppoll(n)

// This is often used to write a raw byte strings, so is not logged for debugging.
void net_write(struct net *n, const char *buf, int len) {
  if(n->state != NETST_ASY || n->syn)
    g_warning("%s: Write in incorrect state.", net_remoteaddr(n));
  else {
    g_string_append_len(n->wbuf, buf, len);
    flush;
  }
}


// Logs the write for debugging. Does not log a trailing newline if there is one.
static void asy_debugwrite(struct net *n, int oldlen) {
  if(n->wbuf->len && n->wbuf->len > oldlen) {
    char end = n->wbuf->str[n->wbuf->len-1];
    if(end == '\n')
      n->wbuf->str[n->wbuf->len-1] = 0;
    g_debug("%s> %s", net_remoteaddr(n), n->wbuf->str+oldlen);
    if(end == '\n')
      n->wbuf->str[n->wbuf->len-1] = end;
  }
}


void net_writestr(struct net *n, const char *msg) {
  if(n->state != NETST_ASY || n->syn)
    g_warning("%s: Writestr in incorrect state: %s", net_remoteaddr(n), msg);
  else {
    int old = n->wbuf->len;
    g_string_append(n->wbuf, msg);
    asy_debugwrite(n, old);
    flush;
  }
}


void net_writef(struct net *n, const char *fmt, ...) {
  if(n->state != NETST_ASY || n->syn)
    g_warning("%s: Writef in incorrect state: %s", net_remoteaddr(n), fmt);
  else {
    int old = n->wbuf->len;
    va_list va;
    va_start(va, fmt);
    g_string_append_vprintf(n->wbuf, fmt, va);
    va_end(va);
    asy_debugwrite(n, old);
    flush;
  }
}

#undef flush


// Switches to the SYN state when the write buffer has been flushed. fd will be
// close()'d when done. cb() will be called in the main thread.
void net_sendfile(struct net *n, int fd, int len, gboolean flush, void (*cb)(struct net *)) {
  g_return_if_fail(n->state == NETST_ASY && !n->syn);
  syn_new(n, TRUE, len);
  n->syn->flush = flush;
  n->syn->cb_upldone = cb;
  n->syn->fd = fd;
  if(!n->wbuf->len)
    syn_start(n);
}


// Clean and orderly shutdown. Callback is called when done (unless there was
// some error). Only supported in the ASY state.
void net_shutdown(struct net *n, void(*cb)(struct net *)) {
  g_return_if_fail(n->state == NETST_ASY);
  g_debug("%s: Shutting down", net_remoteaddr(n));
  n->state = NETST_DIS;
  n->cb_shutdown = cb;
  time(&n->timeout_last);
  if(!n->wbuf->len)
    dis_shutdown(n);
}


// Enables TLS-mode and initates the handshake. May not be called when there's
// something in the write buffer. If the read buffer is not empty, its contents
// are assumed to be valid TLS packets and will be forwarded to gnutls.
// The callback function, if set, will be called when the handshake has
// completed. If a certificate of the peer has been received, its keyprint will
// be sent as first argument. NULL otherwise.
// Once TLS is enabled, it's not possible to switch back to a raw connection
// again.
void net_settls(struct net *n, gboolean serv, void (*cb)(struct net *, const char *)) {
  g_return_if_fail(n->state == NETST_ASY);
  g_return_if_fail(!n->wbuf->len);
  g_return_if_fail(!n->tls);
  if(n->rbuf->len) {
    n->tlsrbuf = n->rbuf;
    n->rbuf = g_string_sized_new(1024);
  }
  gnutls_init(&n->tls, serv ? GNUTLS_SERVER : GNUTLS_CLIENT);
  gnutls_credentials_set(n->tls, GNUTLS_CRD_CERTIFICATE, db_certificate);
  const char *pos;
  gnutls_priority_set_direct(n->tls, var_get(0, VAR_tls_priority), &pos);

  gnutls_transport_set_ptr(n->tls, n);
  gnutls_transport_set_push_function(n->tls, tls_push);
  gnutls_transport_set_pull_function(n->tls, tls_pull);

  n->cb_handshake = cb;
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
  time(&n->timeout_last);
  if(!n->timeout_src)
    n->timeout_src = g_timeout_add_seconds(5, handle_timer, n);

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
  g_debug("%s: Connect error: %s", net_remoteaddr(n), g_strerror(err));
  n->cb_err(n, NETERR_CONN, g_strerror(err));
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

  time(&n->timeout_last);
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
    char *e = g_strdup_printf("Can't bind to local address: %s", g_strerror(errno));
    g_debug("%s: %s", net_remoteaddr(n), e);
    n->cb_err(n, NETERR_CONN, e);
    g_free(e);
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
    g_debug("%s: DNS resolve: %s", net_remoteaddr(n), r->err);
    n->cb_err(n, NETERR_CONN, r->err);
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
  struct addrinfo hint = {};
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

time_t net_last_activity(struct net *n) {
  if(n->syn)
    g_static_mutex_lock(&n->syn->lock);
  time_t last = n->timeout_last;
  if(n->syn)
    g_static_mutex_unlock(&n->syn->lock);
  return last;
}


static gboolean handle_timer(gpointer dat) {
  struct net *n = dat;
  time_t intv = time(NULL)-net_last_activity(n);

  // time() isn't that reliable.
  if(intv < 0) {
    time(&n->timeout_last);
    return TRUE;
  }

  // 30 second timeout on connecting, disconnecting, synchronous transfers, and
  // non-keepalive ASY connections.
  if(intv > 30 && (n->state == NETST_DNS || n->state == NETST_CON || n->state == NETST_DIS || (n->state == NETST_ASY && !n->timeout_msg))) {
    if(n->state == NETST_DNS || n->state == NETST_CON)
      n->cb_err(n, NETERR_TIMEOUT, g_strerror(ETIMEDOUT));
    else {
      g_debug("%s: Timeout.", net_remoteaddr(n));
      n->cb_err(n, NETERR_TIMEOUT, "Idle timeout");
    }
    n->timeout_src = 0;
    return FALSE;
  }

  // For keepalive ASY connections, send the timeout_msg after 2 minutes
  if(intv > 120 && n->state == NETST_ASY && n->timeout_msg)
    net_writestr(n, n->timeout_msg);

  return TRUE;
}


struct net *net_new(void *handle, void(*err)(struct net *, int, const char *)) {
  struct net *n = g_new0(struct net, 1);
  n->ref = 1;
  n->handle = handle;
  n->cb_err = err;
  n->rate_in = g_slice_new0(ratecalc_t);
  n->rate_out = g_slice_new0(ratecalc_t);
  ratecalc_init(n->rate_in);
  ratecalc_init(n->rate_out);
  time(&n->timeout_last);
  return n;
}


// *addr must be a string in the form of 'host:port', where ':port' is
// optional. The callback is called with an address each time a connection
// attempt is made. It is called with NULL when the connection was successful
// (at which point net_remoteaddr() should work).
void net_connect(struct net *n, const char *addr, int defport, const char *laddr, void(*cb)(struct net *, const char *)) {
  g_return_if_fail(n->state == NETST_IDL);

  struct dnscon *r = g_slice_new0(struct dnscon);
  r->addr = str_portsplit(addr, defport, &r->port);
  r->net = n;
  r->cb = cb;

  r->laddr.sin_family = AF_INET;
  r->laddr.sin_port = 0;
  if(laddr)
    inet_pton(AF_INET, laddr, &r->laddr.sin_addr);
  else
    r->laddr.sin_addr.s_addr = INADDR_ANY;

  if(!n->timeout_src)
    n->timeout_src = g_timeout_add_seconds(5, handle_timer, n);
  time(&n->timeout_last);
  n->dnscon = r;
  n->state = NETST_DNS;
  g_thread_pool_push(dns_pool, r, NULL);
}


// Force-disconnect. Can be called from any state.
void net_disconnect(struct net *n) {
  struct synfer *s = NULL;

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
  case NETST_DIS:
    n->rd_cb = NULL;
    if(n->syn) {
      syn_cancel(n);
      syn_free(s);
    }
    break;

  case NETST_SYN:
    s = n->syn;
    if(s)
      syn_cancel(n);
    break;
  }

  // If we're in the SYN state, then the socket and tls session are in control
  // of the file transfer thread. Hence the need for the conditional locks.
  if(s)
    g_static_mutex_lock(&s->lock);
  if(n->tls) {
    gnutls_deinit(n->tls);
    n->tls = NULL;
  }
  if(n->sock) {
    close(n->sock);
    n->sock = 0;
  }
  time(&n->timeout_last);
  if(s)
    g_static_mutex_unlock(&s->lock);

  if(n->rbuf) {
    g_string_free(n->rbuf, TRUE);
    g_string_free(n->wbuf, TRUE);
    n->rbuf = n->wbuf = NULL;
  }

  if(n->tlsrbuf) {
    g_string_free(n->tlsrbuf, TRUE);
    n->tlsrbuf = NULL;
  }

  if(n->socksrc) {
    g_source_remove(n->socksrc);
    n->socksrc = 0;
  }
  if(n->timeout_src) {
    g_source_remove(n->timeout_src);
    n->timeout_src = 0;
  }

  ratecalc_unregister(n->rate_in);
  ratecalc_unregister(n->rate_out);

  if(n->state == NETST_ASY || n->state == NETST_SYN || n->state == NETST_DIS)
    g_debug("%s: Disconnected.", net_remoteaddr(n));
  n->addr[0] = 0;
  n->wantwrite = n->writing = n->tls_handshake = n->shutdown_closed = FALSE;
  n->state = NETST_IDL;
}


void net_unref(struct net *n) {
  if(!g_atomic_int_dec_and_test(&n->ref))
    return;
  g_return_if_fail(n->state == NETST_IDL);
  g_slice_free(ratecalc_t, n->rate_in);
  g_slice_free(ratecalc_t, n->rate_out);
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
  m->msg = g_memdup(msg, len);
  m->msglen = len;
  m->addr.sin_family = AF_INET;
  m->addr.sin_port = htons(port);
  if(inet_pton(AF_INET, ip, &m->addr.sin_addr) != 1) {
    g_debug("UDP: Invalid IP: %s", ip);
    g_free(ip);
    return;
  }
  g_free(ip);

  g_queue_push_tail(net_udp_queue, m);
  if(net_udp_queue->head == net_udp_queue->tail) {
    GSource *src = fdsrc_new(net_udp_sock, G_IO_OUT);
    g_source_set_callback(src, udp_handle_out, NULL, NULL);
    g_source_attach(src, NULL);
    g_source_unref(src);
  }
}


static void net_udp_debug() {
  if(net_udp_queue->tail) {
    struct net_udp *m = net_udp_queue->tail->data;
    char end = m->msglen > 0 ? m->msg[m->msglen-1] : 0;
    if(end == '\n')
      m->msg[m->msglen-1] = 0;
    g_debug("UDP:%s:%d> %s", inet_ntoa(m->addr.sin_addr), ntohs(m->addr.sin_port), m->msg);
    if(end == '\n')
      m->msg[m->msglen-1] = end;
  }
}


void net_udp_send(const char *dest, const char *msg) {
  net_udp_send_raw(dest, msg, strlen(msg));
  net_udp_debug();
}


void net_udp_sendf(const char *dest, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  char *str = g_strdup_vprintf(fmt, va);
  va_end(va);
  net_udp_send_raw(dest, str, strlen(str));
  g_free(str);
  net_udp_debug();
}






// initialize some global structures

void net_init_global() {
  ratecalc_init(&net_in);
  ratecalc_init(&net_out);
  // Don't group these with RCC_UP or RCC_DOWN, otherwise bandwidth will be counted twice.
  ratecalc_register(&net_in, RCC_NONE);
  ratecalc_register(&net_out, RCC_NONE);

  dns_pool = g_thread_pool_new(dnscon_thread, NULL, -1, FALSE, NULL);
  syn_pool = g_thread_pool_new(syn_thread, NULL, -1, FALSE, NULL);

  // TODO: IPv6?
  net_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
  fcntl(net_udp_sock, F_SETFL, fcntl(net_udp_sock, F_GETFL, 0)|O_NONBLOCK);
  net_udp_queue = g_queue_new();
}

