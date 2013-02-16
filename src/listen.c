/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011-2013 Yoran Heling

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
#include "listen.h"


#if INTERFACE

#define LBT_UDP 1
#define LBT_TCP 2

#define LBT_STR(x) ((x) == LBT_TCP ? "TCP" : "UDP")

// port + ip4 are "cached" for convenience.
struct listen_bind_t {
  guint16 type; // LBT_*
  guint16 port;
  struct in_addr ip4;
  int src; // glib event source
  int sock;
  GSList *hubs; // hubs that use this bind
};


struct listen_hub_bind_t {
  guint64 hubid;
  listen_bind_t *tcp, *udp;
};

#endif


GList      *listen_binds     = NULL; // List of currently used binds
GHashTable *listen_hub_binds = NULL; // Map of &hubid to listen_hub_bind

// The port to use when active_port hasn't been set. Initialized to a random
// value on startup. Note that the same port is still used for all hubs that
// have the port set to "random". This obviously isn't very "random", but
// avoids a change to the port every time that listen_refresh() is called.
// Also note that this isn't necessarily a "random *free* port" such as what
// the OS would give if you actually specify port=0, so if a port happens to be
// chosen that is already in use, you'll get an error. (This isn't very nice...
// Especially if the port is being used for e.g. an outgoing connection, which
// isn't very uncommon for high ports).
static guint16 random_tcp_port, random_udp_port;


// Public interface to fetch current listen configuration

gboolean listen_hub_active(guint64 hub) {
  listen_hub_bind_t *b = g_hash_table_lookup(listen_hub_binds, &hub);
  return b && b->tcp;
}

// These all returns 0 if passive or disabled
guint16 listen_hub_tcp(guint64 hub) {
  listen_hub_bind_t *b = g_hash_table_lookup(listen_hub_binds, &hub);
  return b && b->tcp ? b->tcp->port : 0;
}

guint16 listen_hub_udp(guint64 hub) {
  listen_hub_bind_t *b = g_hash_table_lookup(listen_hub_binds, &hub);
  return b && b->udp ? b->udp->port : 0;
}



void listen_global_init() {
  listen_hub_binds = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, g_free);
  random_tcp_port = g_random_int_range(1025, 65535);
  random_udp_port = g_random_int_range(1025, 65535);
}


// Closes all listen sockets and clears *listen_binds and *listen_hub_binds.
static void listen_stop() {
  g_debug("listen: Stopping.");
  g_hash_table_remove_all(listen_hub_binds);
  GList *n, *b = listen_binds;
  while(b) {
    n = b->next;
    listen_bind_t *lb = b->data;
    if(lb->src)
      g_source_remove(lb->src);
    if(lb->sock)
      close(lb->sock);
    g_slist_free(lb->hubs);
    g_free(lb);
    g_list_free_1(b);
    b = n;
  }
  listen_binds = NULL;
}


static gboolean listen_tcp_handle(gpointer dat) {
  listen_bind_t *b = dat;
  struct sockaddr_in a = {};
  socklen_t len = sizeof(a);
  int c = accept(b->sock, (struct sockaddr *)&a, &len);
  fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0)|O_NONBLOCK);

  // handle error
  if(c < 0) {
    if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return TRUE;
    ui_mf(uit_main_tab, 0, "TCP accept error on %s:%d: %s. Switching to passive mode.",
      ip4_unpack(b->ip4), b->port, g_strerror(errno));
    listen_stop();
    hub_global_nfochange();
    return FALSE;
  }

  // Create connection
  char addr_str[100];
  g_snprintf(addr_str, 100, "%s:%d", inet_ntoa(a.sin_addr), ntohs(a.sin_port));
  cc_incoming(cc_create(NULL), b->port, c, addr_str);
  return TRUE;
}


static gboolean listen_udp_handle(gpointer dat) {
  static char buf[5000]; // can be static, this function is only called in the main thread.
  listen_bind_t *b = dat;

  struct sockaddr_in a = {};
  socklen_t len = sizeof(a);
  int r = recvfrom(b->sock, buf, sizeof(buf)-1, 0, (struct sockaddr *)&a, &len);

  // handle error
  if(r < 0) {
    if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return TRUE;
    ui_mf(uit_main_tab, 0, "UDP read error on %s:%d: %s. Switching to passive mode.",
      ip4_unpack(b->ip4), b->port, g_strerror(errno));
    listen_stop();
    hub_global_nfochange();
    return FALSE;
  }

  // Get address in a readable string, for debugging
  char addr_str[100];
  g_snprintf(addr_str, 100, "%s:%d", inet_ntoa(a.sin_addr), ntohs(a.sin_port));

  // Since all incoming messages must be search results, just pass the messages to search.c
  buf[r] = 0;
  if(!search_handle_udp(addr_str, buf, r)) {
    // The message may habe been encrypted with SUDP, so this error reporting
    // may not be too useful.
    g_message("UDP:%s: Invalid message: %s", addr_str, buf);
  }
  return TRUE;
}


#define bind_hub_add(lb, h) do {\
    if((h)->tcp != lb)\
      (lb)->hubs = g_slist_prepend((lb)->hubs, h);\
    if((lb)->type == LBT_TCP)\
      (h)->tcp = lb;\
    else\
      (h)->udp = lb;\
  } while(0)


static void bind_add(listen_hub_bind_t *b, int type, struct in_addr *ip, guint16 port) {
  if(!port)
    port = type == LBT_UDP ? random_udp_port : random_tcp_port;
  g_debug("Listen: Adding %s %s:%d", LBT_STR(type), ip4_unpack(*ip), port);
  // First: look if we can re-use an existing bind and look for any unresolvable conflicts.
  GList *c;
  for(c=listen_binds; c; c=c->next) {
    listen_bind_t *i = c->data;
    // Same? Just re-use.
    if(type == i->type && (ip4_cmp(i->ip4, *ip) == 0 || ip4_isany(i->ip4)) && i->port == port) {
      g_debug("Listen: Re-using!");
      i->type |= type;
      bind_hub_add(i, b);
      return;
    }
  }

  // Create and add bind item
  listen_bind_t *lb = g_new0(listen_bind_t, 1);
  lb->type = type;
  lb->ip4 = *ip;
  lb->port = port;
  bind_hub_add(lb, b);

  // Look for existing binds that should be merged.
  GList *n;
  for(c=listen_binds; ip4_isany(lb->ip4)&&c; c=n) {
    n = c->next;
    listen_bind_t *i = c->data;
    if(i->port != lb->port || i->type != lb->type)
      continue;
    g_debug("Listen: Merging!");
    lb->type |= i->type;
    // Move over all hubs to *lb
    GSList *in;
    for(in=i->hubs; in; in=in->next)
      bind_hub_add(lb, (listen_hub_bind_t *)in->data);
    g_slist_free(i->hubs);
    // And remove this bind
    g_free(i);
    listen_binds = g_list_delete_link(listen_binds, c);
  }

  listen_binds = g_list_prepend(listen_binds, lb);
}


static void bind_create(listen_bind_t *b) {
  g_debug("Listen: binding %s %s:%d", LBT_STR(b->type), ip4_unpack(b->ip4), b->port);
  int err = 0;

  // Create socket
  int sock = socket(AF_INET, b->type != LBT_UDP ? SOCK_STREAM : SOCK_DGRAM, 0);
  g_return_if_fail(sock > 0);

  int set = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&set, sizeof(set));

  fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0)|O_NONBLOCK);

  // Bind
  struct sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(b->port);
  a.sin_addr = b->ip4;
  if(bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0)
    err = errno;

  // listen
  if(!err && b->type != LBT_UDP && listen(sock, 5) < 0)
    err = errno;

  // Bind or listen failed? Abandon ship! (This may be a bit extreme, but at
  // least it avoids any other problems that may arise from a partially
  // activated configuration).
  if(err) {
    ui_mf(uit_main_tab, UIP_MED, "Error binding to %s %s:%d, %s. Switching to passive mode.",
      LBT_STR(b->type), ip4_unpack(b->ip4), b->port, g_strerror(err));
    close(sock);
    listen_stop();
    return;
  }
  b->sock = sock;

  // Start accepting incoming connections or handling incoming messages
  GSource *src = fdsrc_new(sock, G_IO_IN);
  g_source_set_callback((GSource *)src, b->type == LBT_UDP ? listen_udp_handle : listen_tcp_handle, b, NULL);
  b->src = g_source_attach((GSource *)src, NULL);
  g_source_unref((GSource *)src);
}


// Should be called every time a hub is opened/closed or an active_ config
// variable has changed.
void listen_refresh() {
  listen_stop();
  g_debug("listen: Refreshing");

  // Walk through the list of hubs and get their config
  GHashTableIter i;
  hub_t *h = NULL;
  g_hash_table_iter_init(&i, hubs);
  while(g_hash_table_iter_next(&i, NULL, (gpointer *)&h)) {
    // We only look at hubs on which we are active
    if(ip4_isany(hub_ip4(h)) || !var_get_bool(h->id, VAR_active))
      continue;
    // Add to listen_hub_binds
    listen_hub_bind_t *b = g_new0(listen_hub_bind_t, 1);
    b->hubid = h->id;
    g_hash_table_insert(listen_hub_binds, &b->hubid, b);
    // And add the required binds for this hub (Due to the conflict resolution in binds_add(), this is O(n^2))
    // Note: bind_add() can call listen_stop() on error, detect this on whether listen_hub_binds is empty or not.
    struct in_addr localip = ip4_pack(var_get(b->hubid, VAR_local_address));
    bind_add(b, LBT_TCP, &localip, var_get_int(b->hubid, VAR_active_port));
    if(!g_hash_table_size(listen_hub_binds))
      break;
    bind_add(b, LBT_UDP, &localip, var_get_int(b->hubid, VAR_active_udp_port));
    if(!g_hash_table_size(listen_hub_binds))
      break;
  }

  // Now walk through *listen_binds and actually create the listen sockets
  GList *l;
  for(l=listen_binds; l; listen_binds ? (l=l->next) : (l=NULL))
    bind_create((listen_bind_t *)l->data);
}

