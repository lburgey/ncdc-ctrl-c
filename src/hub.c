/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011-2019 Yoran Heling

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
#include "hub.h"


#if INTERFACE

struct hub_user_t {
  gboolean hasinfo : 1;
  gboolean isop : 1;
  gboolean isjoined : 1; // managed by uit_hub_userchange()
  gboolean active : 1;
  gboolean hasudp4 : 1;
  gboolean hasudp6 : 1;
  gboolean hastls : 1;   // NMDC: 0x10 flag in $MyINFO; ADC: SU has ADCS or ADC0
  gboolean hasadc0 : 1;  // (ADC) Whether the SU flag was ADC0 (otherwise it was ADCS)
  unsigned char h_norm;
  unsigned char h_reg;
  unsigned char h_op;
  unsigned char slots;
  unsigned short udp4;
  unsigned short udp6;
  unsigned int as;       // auto-open slot if upload is below n bytes/s
  int sid;        // for ADC
  struct in_addr ip4;
  struct in6_addr ip6;
  hub_t *hub;
  char *name;     // UTF-8
  char *name_hub; // hub-encoded (NMDC)
  char *desc;
  char *conn;     // NMDC: string pointer, ADC: GUINT_TO_POINTER() of the US param
  char *mail;
  char *client;
  guint64 uid;
  guint64 sharesize;
  char *kp;      // ADC with KEYP, 32 bytes slice-alloc'ed
  GSequenceIter *iter; // used by ui_userlist_*
}


struct hub_t {
  gboolean adc : 16;       // TRUE = ADC, FALSE = NMDC protocol.
  gboolean tls : 16;
  int state;               // (ADC) ADC_S_*
  ui_tab_t *tab;
  net_t *net;

  // Hub info / config
  guint64 id;              // "hubid" number
  char *hubname;           // UTF-8, or NULL when unknown
  char *hubname_hub;       // (NMDC) in hub encoding

  // Our user info
  char *nick_hub;          // (NMDC) in hub encoding
  char *nick;              // UTF-8
  int sid;                 // (ADC) session ID
  char *ip;                // Our IP, as received from the hub
  gboolean nick_valid : 1; // TRUE is the above nick has also been validated (and we're properly logged in)
  gboolean isreg : 1;      // whether we used a password to login
  gboolean isop : 1;       // whether we're an OP or not

  // User list information
  int sharecount;
  guint64 sharesize;
  GHashTable *users;       // key = username (in hub encoding for NMDC)
  GHashTable *sessions;    // (ADC) key = sid

  // (NMDC) what we and the hub support
  gboolean supports_nogetinfo;

  // Timers
  guint nfo_timer;         // hub_send_nfo() timer
  guint reconnect_timer;   // reconnect timer

  // ADC login info
  char *gpa_salt;
  int gpa_salt_len;

  // TLS certificate verification
  char *kp;                // NULL if it matches config, 32 bytes slice-alloced otherwise

  // last info we sent to the hub
  char *nfo_desc, *nfo_conn, *nfo_mail, *nfo_ip;
  unsigned char nfo_slots, nfo_free_slots, nfo_h_norm, nfo_h_reg, nfo_h_op;
  guint64 nfo_share;
  guint16 nfo_udp_port;
  gboolean nfo_sup_tls, nfo_sup_sudp;

  // userlist fetching detection
  gboolean received_first;  // true if one precondition for joincomplete is satisfied.
  gboolean joincomplete;    // if we have the userlist
  guint joincomplete_timer; // fallback timer which ensures joincomplete is set at some point
};


#define hub_init_global() do {\
    hub_uids = g_hash_table_new(g_int64_hash, g_int64_equal);\
    hubs = g_hash_table_new(g_int64_hash, g_int64_equal);\
  } while(0)

#endif


// Global hash table of all users, with UID being the index and hub_user struct
// as value.
GHashTable *hub_uids = NULL;

// Global hash table of all opened (but not necessarily connected) hubs, with
// hubid being the index and hub_t as value.
GHashTable *hubs = NULL;


// hub_user_t related functions

/* Generate user ID for ADC */
guint64 hub_user_adc_id(guint64 hubid, const char *cid) {
  tiger_ctx_t t;
  char tmp[MAX(MAXCIDLEN, 24)];
  guint64 uid;

  tiger_init(&t);
  tiger_update(&t, (char *)&hubid, 8);
  base32_decode(cid, tmp);
  tiger_update(&t, tmp, (strlen(cid)*5)/8);
  tiger_final(&t, tmp);
  memcpy(&uid, tmp, 8);

  return uid;
}


guint64 hub_user_nmdc_id(guint64 hubid, const char *name) {
  tiger_ctx_t t;
  guint64 uid;
  char tmp[24];

  tiger_init(&t);
  tiger_update(&t, (char *)&hubid, 8);
  tiger_update(&t, name, strlen(name));
  tiger_final(&t, tmp);
  memcpy(&uid, tmp, 8);
  return uid;
}


// cid is required for ADC. expected to be base32-encoded.
static hub_user_t *user_add(hub_t *hub, const char *name, const char *cid) {
  hub_user_t *u = g_hash_table_lookup(hub->users, name);
  if(u)
    return u;
  u = g_slice_new0(hub_user_t);
  u->hub = hub;
  if(hub->adc) {
    u->name = g_strdup(name);
    u->uid = hub_user_adc_id(hub->id, cid);
  } else {
    u->name_hub = g_strdup(name);
    u->name = charset_convert(hub, TRUE, name);
    u->uid = hub_user_nmdc_id(hub->id, name);
  }
  // insert in hub->users
  g_hash_table_insert(hub->users, hub->adc ? u->name : u->name_hub, u);
  // insert in hub_uids
  if(g_hash_table_lookup(hub_uids, &(u->uid)))
    g_critical("Duplicate user or hash collision for %s @ %s", u->name, hub->tab->name);
  else
    g_hash_table_insert(hub_uids, &(u->uid), u);
  // notify the UI
  uit_hub_userchange(hub->tab, UIHUB_UC_JOIN, u);
  // notify the dl manager
  if(hub->nick_valid)
    dl_user_join(u->uid);
  return u;
}


static void user_free(gpointer dat) {
  hub_user_t *u = dat;
  // remove from hub_uids
  g_hash_table_remove(hub_uids, &(u->uid));
  // remove from hub->sessions
  if(u->hub->adc && u->sid)
    g_hash_table_remove(u->hub->sessions, GINT_TO_POINTER(u->sid));
  // free
  if(u->kp)
    g_slice_free1(32, u->kp);
  g_free(u->name_hub);
  g_free(u->name);
  g_free(u->desc);
  if(!u->hub->adc)
    g_free(u->conn);
  g_free(u->mail);
  g_free(u->client);
  g_slice_free(hub_user_t, u);
}


// Get a user by a UTF-8 string. May fail for NMDC if the UTF-8 -> hub encoding
// is not really one-to-one.
hub_user_t *hub_user_get(hub_t *hub, const char *name) {
  if(hub->adc)
    return g_hash_table_lookup(hub->users, name);
  char *name_hub = charset_convert(hub, FALSE, name);
  hub_user_t *u = g_hash_table_lookup(hub->users, name_hub);
  g_free(name_hub);
  return u;
}


// Auto-complete suggestions for hub_user_get()
void hub_user_suggest(hub_t *hub, char *str, char **sug) {
  GHashTableIter iter;
  hub_user_t *u;
  int i=0, len = strlen(str);
  g_hash_table_iter_init(&iter, hub->users);
  while(i<20 && g_hash_table_iter_next(&iter, NULL, (gpointer *)&u))
    if(g_ascii_strncasecmp(u->name, str, len) == 0)
      sug[i++] = g_strdup(u->name);
  qsort(sug, i, sizeof(char *), cmpstringp);
}



#if INTERFACE

#define hub_user_conn(u) (!(u)->conn ? NULL :\
  (u)->hub->adc ? g_strdup_printf("%d KiB/s", GPOINTER_TO_UINT((u)->conn)/1024) : g_strdup((u)->conn))

#define hub_user_ip(u, def) (!ip4_isany((u)->ip4) ? ip4_unpack((u)->ip4) : !ip6_isany((u)->ip6) ? ip6_unpack((u)->ip6) : def)

#endif


char *hub_user_tag(hub_user_t *u) {
  if(!u->client || !u->slots)
    return NULL;
  GString *t = g_string_new("");
  g_string_printf(t, "<%s,M:%c,H:%d/%d/%d,S:%d", u->client,
    u->active ? 'A' : 'P', u->h_norm, u->h_reg, u->h_op, u->slots);
  if(u->as)
    g_string_append_printf(t, ",O:%d", u->as/1024);
  g_string_append_c(t, '>');
  return g_string_free(t, FALSE);
}


#define cleanspace(str) do {\
    while(*(str) == ' ')\
      (str)++;\
    while((str)[0] && (str)[strlen(str)-1] == ' ')\
      (str)[strlen(str)-1] = 0;\
  } while(0)

static void user_nmdc_nfo(hub_t *hub, hub_user_t *u, char *str) {
  // these all point into *str. *str is modified to contain zeroes in the correct positions
  char *next, *tmp;
  char *desc = NULL;
  char *client = NULL;
  char *conn = NULL;
  char *mail = NULL;
  gboolean active = FALSE;
  unsigned char h_norm = 0;
  unsigned char h_reg = 0;
  unsigned char h_op = 0;
  unsigned char slots = 0;
  unsigned char flags = 0;
  unsigned int as = 0;
  guint64 share = 0;

  if(!(next = strchr(str, '$')) || strlen(next) < 3 || next[2] != '$')
    return;
  if(next[1] == 'A')
    active = TRUE;
  *next = 0; next += 3;

  // tag
  if(str[0] && str[strlen(str)-1] == '>' && (tmp = strrchr(str, '<'))) {
    *tmp = 0;
    tmp++;
    tmp[strlen(tmp)-1] = 0;
    // tmp now points to the contents of the tag
    char *t;

#define L(s) do {\
    if(!client)\
      client = tmp;\
    else if(strcmp(tmp, "M:A") == 0)\
      active = TRUE;\
    else\
      (void) (sscanf(tmp, "H:%hhu/%hhu/%hhu", &h_norm, &h_reg, &h_op)\
      || sscanf(tmp, "S:%hhu", &slots)\
      || sscanf(tmp, "O:%u", &as));\
  } while(0)

    while((t = strchr(tmp, ','))) {
      *t = 0;
      L(tmp);
      tmp = t+1;
    }
    L(tmp);

#undef L
  }

  // description
  desc = str;
  cleanspace(desc);

  // connection and flag
  str = next;
  if(!(next = strchr(str, '$')))
    return;
  if(str != next) {
    flags = *(next-1);
    *(next-1) = 0;
  }
  *next = 0; next++;
  conn = str;
  cleanspace(conn);

  // email
  str = next;
  if(!(next = strchr(str, '$')))
    return;
  *next = 0; next++;

  mail = str;
  cleanspace(mail);

  // share
  str = next;
  if(!(next = strchr(str, '$')))
    return;
  *next = 0;
  share = g_ascii_strtoull(str, NULL, 10);

  // If we still haven't 'return'ed yet, that means we have a correct $MyINFO. Now we can update the struct.
  g_free(u->desc);
  g_free(u->client);
  g_free(u->conn);
  g_free(u->mail);
  u->sharesize = share;
  u->desc = desc[0] ? nmdc_unescape_and_decode(hub, desc) : NULL;
  u->client = client && client[0] ? g_strdup(client) : NULL;
  u->conn = conn[0] ? nmdc_unescape_and_decode(hub, conn) : NULL;
  u->mail = mail[0] ? nmdc_unescape_and_decode(hub, mail) : NULL;
  u->h_norm = h_norm;
  u->h_reg = h_reg;
  u->h_op = h_op;
  u->slots = slots;
  u->as = as*1024;
  u->hasinfo = TRUE;
  u->active = active;
  u->hastls = (flags & 0x10) ? TRUE : FALSE;
  uit_hub_userchange(hub->tab, UIHUB_UC_NFO, u);
}

#undef cleanspace


#define P(a,b) (((a)<<8) + (b))

static void user_adc_nfo(hub_t *hub, hub_user_t *u, adc_cmd_t *cmd) {
  u->hasinfo = TRUE;
  // sid
  if(!u->sid) {
    g_hash_table_insert(hub->sessions, GINT_TO_POINTER(cmd->source), u);
    u->sid = cmd->source;
  }

  // This is faster than calling adc_getparam() each time
  char **n;
  for(n=cmd->argv; n&&*n; n++) {
    if(strlen(*n) < 2)
      continue;
    char *p = *n+2;
    switch(P(**n, (*n)[1])) {
    case P('N','I'): // nick
      g_hash_table_steal(hub->users, u->name);
      g_free(u->name);
      u->name = g_strdup(p);
      g_hash_table_insert(hub->users, u->name, u);
      break;
    case P('D','E'): // description
      g_free(u->desc);
      u->desc = p[0] ? g_strdup(p) : NULL;
      break;
    case P('V','E'): // client name (+ version)
      g_free(u->client);
      char *ap = adc_getparam(cmd->argv, "AP", NULL);
      u->client = !p[0] ? NULL : !ap || strncmp(p, ap, strlen(ap)) == 0 ? g_strdup(p) : g_strdup_printf("%s %s", ap, p);
      break;
    case P('E','M'): // mail
      g_free(u->mail);
      u->mail = p[0] ? g_strdup(p) : NULL;
      break;
    case P('S','S'): // share size
      u->sharesize = g_ascii_strtoull(p, NULL, 10);
      break;
    case P('H','N'): // h_norm
      u->h_norm = strtol(p, NULL, 10);
      break;
    case P('H','R'): // h_reg
      u->h_reg = strtol(p, NULL, 10);
      break;
    case P('H','O'): // h_op
      u->h_op = strtol(p, NULL, 10);
      break;
    case P('S','L'): // slots
      u->slots = strtol(p, NULL, 10);
      break;
    case P('A','S'): // as
      u->slots = strtol(p, NULL, 10);
      break;
    case P('I','4'): // IPv4 address
      u->ip4 = ip4_pack(p);
      break;
    case P('I','6'): // IPv6 address
      u->ip6 = ip6_pack(p);
      break;
    case P('U','4'): // UDP4 port
      u->udp4 = strtol(p, NULL, 10);
      break;
    case P('U','6'): // UDP6 port
      u->udp6 = strtol(p, NULL, 10);
      break;
    case P('S','U'): // supports
      u->active = !!strstr(p, "TCP4") || !!strstr(p, "TCP6");
      u->hasudp4 = !!strstr(p, "UDP4");
      u->hasudp6 = !!strstr(p, "UDP6");
      u->hasadc0 = !!strstr(p, "ADC0");
      u->hastls  = u->hasadc0 || strstr(p, "ADCS");
      break;
    case P('C','T'): // client type (only used to figure out u->isop)
      u->isop = (strtol(p, NULL, 10) & (4 | 8 | 16 | 32)) > 0;
      break;
    case P('U','S'): // upload speed
      u->conn = GUINT_TO_POINTER((int)g_ascii_strtoull(p, NULL, 0));
      break;
    case P('K','P'): // keyprint
      if(u->kp) {
        g_slice_free1(32, u->kp);
        u->kp = NULL;
      }
      if(strncmp(p, "SHA256/", 7) == 0 && strlen(p+7) == 52 && isbase32(p+7)) {
        u->kp = g_slice_alloc(32);
        base32_decode(p+7, u->kp);
      } else
        g_message("Invalid KP field in INF for %s on %s (%s)", u->name, net_remoteaddr(hub->net), p);
      break;
    }
  }

  uit_hub_userchange(hub->tab, UIHUB_UC_NFO, u);
}

#undef P




// hub stuff



hub_t *hub_global_byid(guint64 id) {
  return g_hash_table_lookup(hubs, &id);
}


// Should be called when something changes that may affect our INF or $MyINFO.
void hub_global_nfochange() {
  GHashTableIter i;
  g_hash_table_iter_init(&i, hubs);
  hub_t *h;
  while(g_hash_table_iter_next(&i, NULL, (gpointer *)&h)) {
    if(h->nick_valid)
      hub_send_nfo(h);
  }
}


// Get the current active IP used for this hub. Returns NULL if we're not active.
char *hub_ip(hub_t *hub) {
  if(!net_is_connected(hub->net))
    return NULL;

  char *ip = var_get(hub->id, VAR_active_ip);
  if(ip && strcmp(ip, "local") == 0) {
    ip = (char *)net_localaddr(hub->net);
  } else if(!net_is_ipv6(hub->net)) {
    struct in_addr ip4 = var_parse_ip4(ip);
    ip = ip4_isany(ip4) ? NULL : (char *)ip4_unpack(ip4);
  } else {
    struct in6_addr ip6 = var_parse_ip6(ip);
    ip = ip6_isany(ip6) ? NULL : (char *)ip6_unpack(ip6);
  }

  return ip ? ip : hub->ip;
}


void hub_password(hub_t *hub, char *pass) {
  g_return_if_fail(hub->adc ? hub->state == ADC_S_VERIFY : !hub->nick_valid);

  if(!pass)
    pass = var_get(hub->id, VAR_password);
  if(!pass) {
    ui_m(hub->tab, UIP_HIGH,
      "\nPassword required. Type '/password <your password>' to log in without saving your password."
      "\nOr use '/hset password <your password>' to log in and save your password in the config file (unencrypted!).\n");
  } else if(hub->adc) {
    char enc[40] = {};
    char res[24];
    tiger_ctx_t t;
    tiger_init(&t);
    tiger_update(&t, pass, strlen(pass));
    tiger_update(&t, hub->gpa_salt, hub->gpa_salt_len);
    tiger_final(&t, res);
    base32_encode(res, enc);
    net_writef(hub->net, "HPAS %s\n", enc);
    hub->isreg = TRUE;
  } else {
    net_writef(hub->net, "$MyPass %s|", pass); // Password is sent raw, not encoded. Don't think encoding really matters here.
    hub->isreg = TRUE;
  }
}


void hub_kick(hub_t *hub, hub_user_t *u) {
  g_return_if_fail(!hub->adc && hub->nick_valid && u);
  net_writef(hub->net, "$Kick %s|", u->name_hub);
}


// Initiate a C-C connection with a user
void hub_opencc(hub_t *hub, hub_user_t *u) {
  char token[14] = {};
  if(hub->adc) {
    char nonce[8];
    crypt_nonce(nonce, 8);
    base32_encode_dat(nonce, token, 8);
  }

  guint16 wanttls = var_get_int(hub->id, VAR_tls_policy) == VAR_TLSP_PREFER;
  int port = listen_hub_tcp(hub->id);
  gboolean usetls = wanttls && u->hastls;
  char *adcproto = !usetls ? "ADC/1.0" : u->hasadc0 ? "ADCS/0.10" : "ADCS/1.0";

  // we're active, send CTM
  if(port) {
    if(hub->adc) {
      GString *c = adc_generate('D', ADCC_CTM, hub->sid, u->sid);
      g_string_append_printf(c, " %s %d %s\n", adcproto, port, token);
      net_writef(hub->net, c->str);
      g_string_free(c, TRUE);
    } else
      net_writef(hub->net, net_is_ipv6(hub->net) ? "$ConnectToMe %s [%s]:%d%s" : "$ConnectToMe %s %s:%d%s|",
          u->name_hub, hub_ip(hub), port, usetls ? "S" : "");

  // we're passive, send RCM
  } else {
    if(hub->adc) {
      GString *c = adc_generate('D', ADCC_RCM, hub->sid, u->sid);
      g_string_append_printf(c, " %s %s\n", adcproto, token);
      net_writestr(hub->net, c->str);
      g_string_free(c, TRUE);
    } else // Can't specify TLS preference in $RevConnectToMe :(
      net_writef(hub->net, "$RevConnectToMe %s %s|", hub->nick_hub, u->name_hub);
  }

  cc_expect_add(hub, u, port, hub->adc ? token : NULL, TRUE);
}


// Send a search request
void hub_search(hub_t *hub, search_q_t *q) {
  // ADC
  if(hub->adc) {
    // TODO: use FSCH to only get results from active users when we are passive?
    GString *cmd = adc_generate('B', ADCC_SCH, hub->sid, 0);
    if(listen_hub_active(hub->id)) {
      char token[14] = {};
      base32_encode_dat((char *)&hub->id, token, 8);
      g_string_append(cmd, " TO");
      g_string_append(cmd, token);
    }
    if(q->type == 9) {
      char tth[40] = {};
      base32_encode(q->tth, tth);
      g_string_append_printf(cmd, " TR%s", tth);
    } else {
      if(q->size)
        g_string_append_printf(cmd, " %s%"G_GUINT64_FORMAT, q->ge ? "GE" : "LE", q->size);
      if(q->type == 8)
        g_string_append(cmd, " TY2");
      else if(q->type != 1) {
        char **e = search_types[(int)q->type].exts;
        for(; *e; e++)
          g_string_append_printf(cmd, " EX%s", *e);
        g_string_append(cmd, " TY1");
      }
      char **s = q->query;
      for(; *s; s++)
        g_string_append_printf(cmd, " AN%s", *s);
    }
    if(hub->tls && var_get_int(0, VAR_sudp_policy) == VAR_SUDPP_PREFER) {
      char key[27] = {};
      base32_encode_dat(q->key, key, 16);
      g_string_append_printf(cmd, " KY%s", key);
    }
    g_string_append_c(cmd, '\n');
    net_writestr(hub->net, cmd->str);
    g_string_free(cmd, TRUE);

  // NMDC
  } else {
    guint16 udpport = listen_hub_udp(hub->id);
    char *dest = udpport
      ? g_strdup_printf(net_is_ipv6(hub->net) ? "[%s]:%d" : "%s:%d", hub_ip(hub), udpport)
      : g_strdup_printf("Hub:%s", hub->nick_hub);
    if(q->type == 9) {
      char tth[40] = {};
      base32_encode(q->tth, tth);
      net_writef(hub->net, "$Search %s F?T?0?9?TTH:%s|", dest, tth);
    } else {
      char *str = g_strjoinv(" ", q->query);
      char *enc = nmdc_encode_and_escape(hub, str);
      g_free(str);
      for(str=enc; *str; str++)
        if(*str == ' ')
          *str = '$';
      net_writef(hub->net, "$Search %s %c?%c?%"G_GUINT64_FORMAT"?%d?%s|",
        dest, q->size ? 'T' : 'F', q->ge ? 'F' : 'T', q->size, q->type, enc);
      g_free(enc);
    }
    g_free(dest);
  }
}


#define streq(a) ((!a && !hub->nfo_##a) || (a && hub->nfo_##a && strcmp(a, hub->nfo_##a) == 0))
#define eq(a) (a == hub->nfo_##a)
#define beq(a) (!!a == !!hub->nfo_##a)

static unsigned char num_free_slots(unsigned char hub_slots) {
  int rv = hub_slots - cc_slots_in_use(NULL);
  return rv > 0 ? rv : 0;
}

static GString* format_desc(hub_t *hub, unsigned char free_slots) {
  GString *desc;
  const char *static_desc = var_get(hub->id, VAR_description);
  if(var_get_bool(hub->id, VAR_show_free_slots)) {
    desc = g_string_sized_new(128);
    if(static_desc)
      g_string_printf(desc, "[%d sl] %s", free_slots, static_desc);
    else
      g_string_printf(desc, "[%d sl]", free_slots);
  } else
    desc = g_string_new(static_desc);
  return desc;
}

void hub_send_nfo(hub_t *hub) {
  if(!net_is_connected(hub->net))
    return;

  // get info, to be compared with hub->nfo_
  char *desc, *conn = NULL, *mail, *ip;
  unsigned char slots, free_slots, h_norm, h_reg, h_op;
  guint64 share;
  guint16 udp_port;
  gboolean sup_tls, sup_sudp;
  GString *fmt_desc;

  mail = var_get(hub->id, VAR_email);
  slots = var_get_int(0, VAR_slots);
  free_slots = num_free_slots(slots);
  fmt_desc = format_desc(hub, free_slots);
  desc = fmt_desc->str;

  char buf[50] = {};
  if(var_get_int(0, VAR_upload_rate)) {
    g_snprintf(buf, sizeof(buf), "%d KiB/s", var_get_int(0, VAR_upload_rate)/1024);
    conn = buf;
  } else
    conn = var_get(hub->id, VAR_connection);

  h_norm = h_reg = h_op = 0;
  GHashTableIter iter;
  hub_t *oh = NULL;
  g_hash_table_iter_init(&iter, hubs);
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&oh)) {
    if(!oh->nick_valid)
      continue;
    if(oh->isop)
      h_op++;
    else if(oh->isreg)
      h_reg++;
    else
      h_norm++;
  }
  ip = listen_hub_active(hub->id) ? hub_ip(hub) : NULL;
  udp_port = listen_hub_udp(hub->id);
  share = fl_local_list_size;
  sup_tls = var_get_int(hub->id, VAR_tls_policy) > VAR_TLSP_DISABLE ? TRUE : FALSE;
  sup_sudp = hub->tls && var_get_int(0, VAR_sudp_policy) != VAR_SUDPP_DISABLE ? TRUE : FALSE;

  // check whether we need to make any further effort
  if(hub->nick_valid && streq(desc) && streq(conn) && streq(mail) && eq(slots) && eq(free_slots) && streq(ip)
      && eq(h_norm) && eq(h_reg) && eq(h_op) && eq(share) && eq(udp_port) && beq(sup_tls) && beq(sup_sudp)) {
    g_string_free(fmt_desc, TRUE);
    return;
  }

  char *nfo;
  // ADC
  if(hub->adc) { // TODO: DS and SF?
    GString *cmd = adc_generate('B', ADCC_INF, hub->sid, 0);
    // send non-changing stuff in the IDENTIFY state
    gboolean f = hub->state == ADC_S_IDENTIFY;
    if(f) {
      g_string_append_printf(cmd, " ID%s PD%s VEncdc\\s%s", var_get(0, VAR_cid), var_get(0, VAR_pid), main_version);
      adc_append(cmd, "NI", hub->nick);
      // Always add our KP field, even if we're not active. Other clients may
      // validate our certificate even when we are the one connecting.
      g_string_append_printf(cmd, " KPSHA256/%s", db_certificate_kp);
    }
    if(f || !streq(ip))
      g_string_append_printf(cmd, net_is_ipv6(hub->net) ? " I6%s" : " I4%s", ip ? ip : !net_is_ipv6(hub->net) ? ip4_unpack(ip4_any) : ip6_unpack(ip6_any));
    if(f || !streq(ip) || !beq(sup_tls) || !beq(sup_sudp)) {
      g_string_append(cmd, " SU");
      int comma = 0;
      if(ip)
        g_string_append_printf(cmd, "%s%s", comma++ ? "," : "", net_is_ipv6(hub->net) ? "TCP6,UDP6" : "TCP4,UDP4");
      if(sup_tls)
        g_string_append_printf(cmd, "%s%s", comma++ ? "," : "", "ADC0,ADCS");
      if(sup_sudp)
        g_string_append_printf(cmd, "%s%s", comma++ ? "," : "", "SUD1,SUDP");
    }
    if((f || !eq(udp_port))) {
      if(udp_port)
        g_string_append_printf(cmd, net_is_ipv6(hub->net) ? " U6%d" : " U4%d", udp_port);
      else
        g_string_append(cmd, net_is_ipv6(hub->net) ? " U6" : " U4");
    }
    // Separating the SS and SF fields isn't very important. It is relatively
    // safe to assume that if SS changes (which we look at), then SF will most
    // likely have changed as well. And vice versa.
    if(f || !eq(share))
      g_string_append_printf(cmd, " SS%"G_GUINT64_FORMAT" SF%d", share, fl_local_list_length);
    if(f || !eq(slots))
      g_string_append_printf(cmd, " SL%d", slots);
    if(f || !eq(free_slots))
      g_string_append_printf(cmd, " FS%d", free_slots);
    if(f || !eq(h_norm))
      g_string_append_printf(cmd, " HN%d", h_norm);
    if(f || !eq(h_reg))
      g_string_append_printf(cmd, " HR%d", h_reg);
    if(f || !eq(h_op))
      g_string_append_printf(cmd, " HO%d", h_op);
    if(f || !streq(desc))
      adc_append(cmd, "DE", desc?desc:"");
    if(f || !streq(mail))
      adc_append(cmd, "EM", mail?mail:"");
    if((f || !streq(conn)) && str_connection_to_speed(conn))
      g_string_append_printf(cmd, " US%"G_GUINT64_FORMAT, str_connection_to_speed(conn));
    g_string_append_c(cmd, '\n');
    nfo = g_string_free(cmd, FALSE);

  // NMDC
  } else {
    char *ndesc = nmdc_encode_and_escape(hub, desc?desc:"");
    char *nconn = nmdc_encode_and_escape(hub, conn?conn:"0.005");
    char *nmail = nmdc_encode_and_escape(hub, mail?mail:"");
    nfo = g_strdup_printf("$MyINFO $ALL %s %s<ncdc V:%s,M:%c,H:%d/%d/%d,S:%d>$ $%s%c$%s$%"G_GUINT64_FORMAT"$|",
      hub->nick_hub, ndesc, main_version, ip ? 'A' : 'P', h_norm, h_reg, h_op,
      slots, nconn, 1 | (sup_tls ? 0x10 : 0), nmail, share);
    g_free(ndesc);
    g_free(nconn);
    g_free(nmail);
  }

  // send
  net_writestr(hub->net, nfo);
  g_free(nfo);

  // update
  g_free(hub->nfo_desc); hub->nfo_desc = g_string_free(fmt_desc, FALSE);
  g_free(hub->nfo_conn); hub->nfo_conn = g_strdup(conn);
  g_free(hub->nfo_mail); hub->nfo_mail = g_strdup(mail);
  g_free(hub->nfo_ip);   hub->nfo_ip   = g_strdup(ip);
  hub->nfo_slots = slots;
  hub->nfo_free_slots = free_slots;
  hub->nfo_h_norm = h_norm;
  hub->nfo_h_reg = h_reg;
  hub->nfo_h_op = h_op;
  hub->nfo_share = share;
  hub->nfo_udp_port = udp_port;
  hub->nfo_sup_tls = sup_tls;
  hub->nfo_sup_sudp = sup_sudp;
}

#undef eq
#undef streq


void hub_say(hub_t *hub, const char *str, gboolean me) {
  if(!hub->nick_valid)
    return;
  if(hub->adc) {
    GString *c = adc_generate('B', ADCC_MSG, hub->sid, 0);
    adc_append(c, NULL, str);
    if(me)
      g_string_append(c, " ME1");
    g_string_append_c(c, '\n');
    net_writestr(hub->net, c->str);
    g_string_free(c, TRUE);
  } else {
    char *msg = nmdc_encode_and_escape(hub, str);
    net_writef(hub->net, me ? "<%s> /me %s|" : "<%s> %s|", hub->nick_hub, msg);
    g_free(msg);
  }
}


void hub_msg(hub_t *hub, hub_user_t *user, const char *str, gboolean me) {
  if(hub->adc) {
    GString *c = adc_generate('E', ADCC_MSG, hub->sid, user->sid);
    adc_append(c, NULL, str);
    char enc[5] = {};
    ADC_EFCC(hub->sid, enc);
    g_string_append_printf(c, " PM%s", enc);
    if(me)
      g_string_append(c, " ME1");
    g_string_append_c(c, '\n');
    net_writestr(hub->net, c->str);
    g_string_free(c, TRUE);
  } else {
    char *msg = nmdc_encode_and_escape(hub, str);
    net_writef(hub->net, me ? "$To: %s From: %s $<%s> /me %s|" : "$To: %s From: %s $<%s> %s|",
      user->name_hub, hub->nick_hub, hub->nick_hub, msg);
    g_free(msg);
    // emulate protocol echo
    msg = g_strdup_printf(me ? "<%s> /me %s" : "<%s> %s", hub->nick, str);
    uit_msg_msg(user, msg);
    g_free(msg);
  }
}


// Call this when the hub tells us our IP.
static void setownip(hub_t *hub, hub_user_t *u) {
  char *oldconf = hub_ip(hub);
  char *oldval = hub->ip;
  char *new = net_is_ipv6(hub->net)
    ? (ip6_isany(u->ip6) ? NULL : (char *)ip6_unpack(u->ip6))
    : (ip4_isany(u->ip4) ? NULL : (char *)ip4_unpack(u->ip4));
  if((!new && oldval) || (new && !oldval) || (new && oldval && strcmp(new, oldval) != 0)) {
    g_free(hub->ip);
    hub->ip = g_strdup(new);
  }

  // If we're supposed to be active, but weren't because of a missing IP.
  if(!oldconf && new && var_get_bool(hub->id, VAR_active)) {
    listen_refresh();
    hub_send_nfo(hub);
  }
}


static void adc_sch_reply_send(hub_t *hub, net_udp_t *udp, GString *r, const char *key) {
  if(!udp) {
    net_writestr(hub->net, r->str);
    return;
  } else if(!key || var_get_int(0, VAR_sudp_policy) == VAR_SUDPP_DISABLE) {
    net_udp_send(udp, r->str);
    return;
  }

  // net_udp_* can't log this since it will be encrypted, so log it here.
  g_debug("SUDP:%s> %.*s", udp->addr, (int)r->len-1, r->str);

  // prepend 16 random bytes to message
  char nonce[16];
  crypt_nonce(nonce, 16);
  g_string_prepend_len(r, nonce, 16);

  // use PKCS#5 padding to align the message length to the cypher block size (16)
  int pad = 16 - (r->len & 15);
  int i;
  for(i=0; i<pad; i++)
    g_string_append_c(r, pad);

  // Now encrypt & send
  crypt_aes128cbc(TRUE, key, 16, r->str, r->len);
  net_udp_send_raw(udp, r->str, r->len);
}


static void adc_sch_reply(hub_t *hub, adc_cmd_t *cmd, hub_user_t *u, fl_list_t **res, int len) {
  char *ky = adc_getparam(cmd->argv, "KY", NULL); // SUDP key
  char *to = adc_getparam(cmd->argv, "TO", NULL); // token

  char sudpkey[16];
  if(ky && isbase32(ky) && strlen(ky) == 26)
    base32_decode(ky, sudpkey);

  int slots = var_get_int(0, VAR_slots);
  int slots_free = slots - cc_slots_in_use(NULL);
  if(slots_free < 0)
    slots_free = 0;

  char tth[40] = {};
  char *cid = NULL;
  net_udp_t udpbuf, *udp = NULL;
  if((u->hasudp4 && u->udp4) || (u->hasudp6 && u->udp6)) {
    cid = var_get(0, VAR_cid);
    /* TODO: If the user has both UDP4 and UDP6, we should prefer the AF used
     * to connect to the hub, rather than IPv4. */
    udp = &udpbuf;
    net_udp_init(udp,
        u->hasudp4 && u->udp4 ? ip4_unpack(u->ip4) : ip6_unpack(u->ip6),
        u->hasudp4 && u->udp4 ? u->udp4 : u->udp6,
        var_get(hub->id, VAR_local_address)
    );
  }

  int i;
  for(i=0; i<len; i++) {
    GString *r = udp ? adc_generate('U', ADCC_RES, 0, 0) : adc_generate('D', ADCC_RES, hub->sid, cmd->source);
    if(udp)
      g_string_append_printf(r, " %s", cid);
    if(to)
      adc_append(r, "TO", to);
    g_string_append_printf(r, " SL%d SI%"G_GUINT64_FORMAT, slots_free, res[i]->size);
    char *path = fl_list_path(res[i]);
    adc_append(r, "FN", path);
    g_free(path);
    if(res[i]->isfile) {
      base32_encode(res[i]->tth, tth);
      g_string_append_printf(r, " TR%s", tth);
    } else
      g_string_append_c(r, '/'); // make sure a directory path ends with a slash
    g_string_append_c(r, '\n');

    adc_sch_reply_send(hub, udp, r, ky ? sudpkey : NULL);
    g_string_free(r, TRUE);
  }

  if(udp)
    net_udp_destroy(udp);
}


static void adc_sch(hub_t *hub, adc_cmd_t *cmd) {
  char *an = adc_getparam(cmd->argv, "AN", NULL); // and
  char *no = adc_getparam(cmd->argv, "NO", NULL); // not
  char *ex = adc_getparam(cmd->argv, "EX", NULL); // ext
  char *le = adc_getparam(cmd->argv, "LE", NULL); // less-than
  char *ge = adc_getparam(cmd->argv, "GE", NULL); // greater-than
  char *eq = adc_getparam(cmd->argv, "EQ", NULL); // equal
  char *ty = adc_getparam(cmd->argv, "TY", NULL); // type (1=file, 2=dir)
  char *tr = adc_getparam(cmd->argv, "TR", NULL); // TTH root
  char *td = adc_getparam(cmd->argv, "TD", NULL); // tree depth

  // no strong enough filters specified? ignore
  if(!an && !no && !ex && !le && !ge && !eq && !tr)
    return;

  // le, ge and eq are mutually exclusive (actually, they aren't when they're all equal, but it's still silly)
  if((eq?1:0) + (le?1:0) + (ge?1:0) > 1)
    return;

  // Actually matching the tree depth is rather resouce-intensive, since we
  // don't store it in fl_list_t. Instead, just assume fl_hash_keep_level
  // for everything. This may be wrong, but if it is, it's most likely to be a
  // pessimistic estimate, which happens in the case TTH data is converted from
  // a DC++ client to ncdc.
  if(td && strtoll(td, NULL, 10) > fl_hash_keep_level)
    return;

  if(tr && !istth(tr))
    return;

  hub_user_t *u = g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(cmd->source));
  if(!u)
    return;

  // create search struct
  fl_search_t s = {};
  s.sizem = eq ? 0 : le ? -1 : ge ? 1 : -2;
  s.size = s.sizem == -2 ? 0 : g_ascii_strtoull(eq ? eq : le ? le : ge, NULL, 10);
  s.filedir = !ty ? 3 : ty[0] == '1' ? 1 : 2;
  char **tmp = adc_getparams(cmd->argv, "AN");
  s.and = fl_search_create_and(tmp);
  g_free(tmp);
  tmp = adc_getparams(cmd->argv, "NO");
  s.not = fl_search_create_not(tmp);
  g_free(tmp);
  s.ext = adc_getparams(cmd->argv, "EX");

  int i = 0;
  int max = (u->hasudp4 && u->udp4) || (u->hasudp6 && u->udp6) ? 10 : 5;
  fl_list_t *res[max];

  // TTH lookup
  if(tr) {
    char root[24];
    base32_decode(tr, root);
    GSList *l = fl_local_from_tth(root);
    // it still has to match the other requirements...
    for(; i<max && l; l=l->next) {
      fl_list_t *c = l->data;
      if(fl_search_match_full(c, &s))
        res[i++] = c;
    }

  // Advanced lookup (Noo! This is slooow!)
  } else
    i = fl_search_rec(fl_local_list, &s, res, max);

  if(i)
    adc_sch_reply(hub, cmd, u, res, i);

  fl_search_free_and(s.and);
  if(s.not)
    g_regex_unref(s.not);
  g_free(s.ext);
}


// Many ways to say the same thing
#define is_adcs_proto(p)  (strcmp(p, "ADCS/1.0") == 0 || strcmp(p, "ADCS/0.10") == 0 || strcmp(p, "ADC0/0.10") == 0)
#define is_adc_proto(p)   (strcmp(p, "ADC/1.0") == 0  || strcmp(p, "ADC/0.10") == 0)
#define is_valid_proto(p) (is_adc_proto(p) || is_adcs_proto(p))

static void adc_handle(net_t *net, char *msg, int _len) {
  hub_t *hub = net_handle(net);
  net_readmsg(net, '\n', adc_handle);

  adc_cmd_t cmd;
  GError *err = NULL;

  if(!msg[0])
    return;

  int feats[2] = {};
  if(listen_hub_active(hub->id))
    feats[0] = ADC_DFCC("TCP4");

  adc_parse(msg, &cmd, feats, &err);
  if(err) {
    g_message("ADC parse error from %s: %s. --> %s", net_remoteaddr(hub->net), err->message, msg);
    g_error_free(err);
    return;
  }

  switch(cmd.cmd) {
  case ADCC_SID:
    if(hub->state != ADC_S_PROTOCOL || cmd.type != 'I' || cmd.argc != 1 || strlen(cmd.argv[0]) != 4)
      g_message("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    else {
      hub->sid = ADC_DFCC(cmd.argv[0]);
      hub->state = ADC_S_IDENTIFY;
      hub->nick = g_strdup(var_get(hub->id, VAR_nick));
      uit_hub_setnick(hub->tab);
      hub_send_nfo(hub);
    }
    break;

  case ADCC_SUP:
    // TODO: do something with it.
    // For C-C connections, this enables the IDENTIFY state, but for hubs it's the SID command that does this.
    break;

  case ADCC_INF:
    // inf from hub
    if(cmd.type == 'I') {
      // Get hub name. Some hubs (PyAdc) send multiple 'NI's, ignore the first
      // one in that case. Other hubs don't send 'NI', but only a 'DE'.
      char **left = NULL;
      char *hname = adc_getparam(cmd.argv, "NI", &left);
      if(left)
        hname = adc_getparam(left, "NI", NULL);
      if(!hname)
        hname = adc_getparam(cmd.argv, "DE", NULL);
      if(hname) {
        g_free(hub->hubname);
        hub->hubname = g_strdup(hname);
      }
    // inf from user
    } else if(cmd.type == 'B') {
      hub_user_t *u = g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(cmd.source));
      if(!u) {
        char *nick = adc_getparam(cmd.argv, "NI", NULL);
        char *cid = adc_getparam(cmd.argv, "ID", NULL);
        if(nick && cid && iscid(cid))
          u = user_add(hub, nick, cid);
      }
      if(!u)
        g_message("INF for user who is not on the hub (%s): %s", net_remoteaddr(hub->net), msg);
      else {
        if(!u->hasinfo)
          hub->sharecount++;
        else
          hub->sharesize -= u->sharesize;
        user_adc_nfo(hub, u, &cmd);
        hub->sharesize += u->sharesize;
        // if we received our own INF, that means the user list is complete and
        // we are properly logged in.
        if(u->sid == hub->sid) {
          hub->state = ADC_S_NORMAL;
          hub->isop = u->isop;
          if(!hub->nick_valid)
            dl_user_join(0);
          hub->nick_valid = TRUE;
          hub->joincomplete = TRUE;
          // This means we also have an IP, probably
          setownip(hub, u);
        }
      }
    }
    break;

  case ADCC_QUI:
    if(cmd.type != 'I' || cmd.argc < 1 || strlen(cmd.argv[0]) != 4)
      g_message("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    else {
      int sid = ADC_DFCC(cmd.argv[0]);
      hub_user_t *u = g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(sid));
      if(sid == hub->sid) {
        char *rd = adc_getparam(cmd.argv, "RD", NULL);
        char *ms = adc_getparam(cmd.argv, "MS", NULL);
        char *tl = adc_getparam(cmd.argv, "TL", NULL);
        if(rd) {
          ui_mf(hub->tab, UIP_HIGH, "\nThe hub is requesting you to move to %s.\nType `/connect %s' to do so.\n", rd, rd);
          if(ms)
            ui_mf(hub->tab, 0, "Message: %s", ms);
        } else if(ms)
          ui_m(hub->tab, UIP_MED, ms);
        hub_disconnect(hub, rd || (tl && strcmp(tl, "-1") == 0) ? FALSE : TRUE);
      } else if(u) { // TODO: handle DI, and perhaps do something with MS
        uit_hub_userchange(hub->tab, UIHUB_UC_QUIT, u);
        hub->sharecount--;
        hub->sharesize -= u->sharesize;
        g_hash_table_remove(hub->users, u->name);
      } else
        g_message("QUI for user who is not on the hub (%s): %s", net_remoteaddr(hub->net), msg);
    }
    break;

  case ADCC_STA:
    if(cmd.argc < 2 || strlen(cmd.argv[0]) != 3)
      g_message("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    else {
      int code = (cmd.argv[0][1]-'0')*10 + (cmd.argv[0][2]-'0');
      int severity = cmd.argv[0][0]-'0';
      /* Direct STA messages are usually from CTM/RCM errors. Passing those to
       * the main chat is spammy and unintuitive. */
      if(cmd.type != 'D')
        ui_mf(hub->tab, UIP_LOW, "(%s-%02d) %s",
          !severity ? "status" : severity == 1 ? "warning" : "error", code, cmd.argv[1]);
      else {
        hub_user_t *u = g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(cmd.source));
        g_message("Direct Status message from %s on %s: %s", u ? u->name : "unknown user", net_remoteaddr(hub->net), msg);
      }
    }
    break;

  case ADCC_CTM:
    if(cmd.argc < 3 || cmd.type != 'D' || cmd.dest != hub->sid)
      g_message("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    else if(var_get_int(hub->id, VAR_tls_policy) == VAR_TLSP_DISABLE ? !is_adc_proto(cmd.argv[0]) : !is_valid_proto(cmd.argv[0])) {
      GString *r = adc_generate('D', ADCC_STA, hub->sid, cmd.source);
      g_string_append(r, " 141 Unknown\\sprotocol");
      adc_append(r, "PR", cmd.argv[0]);
      adc_append(r, "TO", cmd.argv[2]);
      g_string_append_c(r, '\n');
      net_writestr(hub->net, r->str);
      g_string_free(r, TRUE);
    } else {
      hub_user_t *u = g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(cmd.source));
      int port = strtol(cmd.argv[1], NULL, 0);
      if(!u)
        g_message("CTM from user who is not on the hub (%s): %s", net_remoteaddr(hub->net), msg);
      else if(port < 1 || port > 65535)
        g_message("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
      else if(!u->active || (ip4_isany(u->ip4) && ip6_isany(u->ip6))) {
        g_message("CTM from user who is not active (%s): %s", net_remoteaddr(hub->net), msg);
        GString *r = adc_generate('D', ADCC_STA, hub->sid, cmd.source);
        g_string_append(r, " 140 No\\sIP\\sto\\sconnect\\sto.\n");
        net_writestr(hub->net, r->str);
        g_string_free(r, TRUE);
      } else
        cc_adc_connect(cc_create(hub), u, var_get(hub->id, VAR_local_address), port, is_adcs_proto(cmd.argv[0]), cmd.argv[2]);
    }
    break;

  case ADCC_RCM:
    if(cmd.argc < 2 || cmd.type != 'D' || cmd.dest != hub->sid)
      g_message("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    else if(var_get_int(hub->id, VAR_tls_policy) == VAR_TLSP_DISABLE ? !is_adc_proto(cmd.argv[0]) : !is_valid_proto(cmd.argv[0])) {
      GString *r = adc_generate('D', ADCC_STA, hub->sid, cmd.source);
      g_string_append(r, " 141 Unknown\\protocol");
      adc_append(r, "PR", cmd.argv[0]);
      adc_append(r, "TO", cmd.argv[1]);
      g_string_append_c(r, '\n');
      net_writestr(hub->net, r->str);
      g_string_free(r, TRUE);
    } else if(!listen_hub_active(hub->id)) {
      GString *r = adc_generate('D', ADCC_STA, hub->sid, cmd.source);
      g_string_append(r, " 142 Not\\sactive");
      adc_append(r, "PR", cmd.argv[0]);
      adc_append(r, "TO", cmd.argv[1]);
      g_string_append_c(r, '\n');
      net_writestr(hub->net, r->str);
      g_string_free(r, TRUE);
    } else {
      hub_user_t *u = g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(cmd.source));
      if(!u)
        g_message("RCM from user who is not on the hub (%s): %s", net_remoteaddr(hub->net), msg);
      else {
        int port = listen_hub_tcp(hub->id);
        GString *r = adc_generate('D', ADCC_CTM, hub->sid, cmd.source);
        adc_append(r, NULL, cmd.argv[0]);
        g_string_append_printf(r, " %d", port);
        adc_append(r, NULL, cmd.argv[1]);
        g_string_append_c(r, '\n');
        net_writestr(hub->net, r->str);
        g_string_free(r, TRUE);
        cc_expect_add(hub, u, port, cmd.argv[1], FALSE);
      }
    }
    break;

  case ADCC_MSG:;
    if(cmd.argc < 1 || (cmd.type != 'B' && cmd.type != 'E' && cmd.type != 'D' && cmd.type != 'I' && cmd.type != 'F'))
      g_message("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    else {
      char *pm = adc_getparam(cmd.argv+1, "PM", NULL);
      gboolean me = adc_getparam(cmd.argv+1, "ME", NULL) != NULL;
      hub_user_t *u = cmd.type != 'I' ? g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(cmd.source)) : NULL;
      hub_user_t *d = (cmd.type == 'E' || cmd.type == 'D') && cmd.source == hub->sid
        ? g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(cmd.dest)) : NULL;
      if(cmd.type != 'I' && !u && !d)
        g_message("Message from someone not on this hub. (%s: %s)", net_remoteaddr(hub->net), msg);
      else {
        char *m = g_strdup_printf(me ? "** %s %s" : "<%s> %s", cmd.type == 'I' ? "hub" : u->name, cmd.argv[0]);
        if(cmd.type == 'E' || cmd.type == 'D' || cmd.type == 'F') { // PM
          hub_user_t *pmu = pm && strlen(pm) == 4 && ADC_DFCC(pm) ? g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(ADC_DFCC(pm))) : NULL;
          if(!pmu)
            g_message("Invalid PM param in MSG from %s: %s", net_remoteaddr(hub->net), msg);
          else
            uit_msg_msg(cmd.source == hub->sid ? d : pmu, m);
        } else // hub chat
          ui_m(hub->tab, UIM_CHAT|UIP_MED, m);
        g_free(m);
      }
    }
    break;

  case ADCC_SCH:
    if(cmd.type != 'B' && cmd.type != 'D' && cmd.type != 'E' && cmd.type != 'F')
      g_message("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    else if(cmd.source != hub->sid)
      adc_sch(hub, &cmd);
    break;

  case ADCC_GPA:
    if(cmd.type != 'I' || cmd.argc < 1 || (hub->state != ADC_S_IDENTIFY && hub->state != ADC_S_VERIFY))
      g_message("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    else {
      g_free(hub->gpa_salt);
      hub->state = ADC_S_VERIFY;
      hub->gpa_salt_len = (strlen(cmd.argv[0])*5)/8;
      hub->gpa_salt = g_new(char, hub->gpa_salt_len);
      base32_decode(cmd.argv[0], hub->gpa_salt);
      hub_password(hub, NULL);
    }
    break;

  case ADCC_RES:
    if(cmd.type != 'D' || cmd.argc < 3)
      g_message("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    else if(!search_handle_adc(hub, &cmd))
      g_message("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    break;

  case ADCC_GET:
    if(cmd.type != 'I' || cmd.argc < 4 || strcmp(cmd.argv[0], "blom") != 0 || strcmp(cmd.argv[1], "/") != 0 || strcmp(cmd.argv[2], "0") != 0)
      g_message("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    else {
      char *bk = adc_getparam(cmd.argv+4, "BK", NULL);
      char *bh = adc_getparam(cmd.argv+4, "BH", NULL);
      long m = strtol(cmd.argv[3], NULL, 10);
      long k = bk ? strtol(bk, NULL, 10) : 0;
      long h = bh ? strtol(bh, NULL, 10) : 0;
      bloom_t b;
      if(bloom_init(&b, m, k, h) < 0)
        g_message("Invalid bloom filter parameters from %s: %s", net_remoteaddr(hub->net), msg);
      else {
        fl_local_bloom(&b);
        GString *r = adc_generate('H', ADCC_SND, 0, 0);
        g_string_append_printf(r, " blom / 0 %d\n", b.m);
        net_writestr(hub->net, r->str);
        g_string_free(r, TRUE);
        net_write(hub->net, (char *)b.d, b.m);
        bloom_free(&b);
      }
    }
    break;

  default:
    g_message("Unknown command from %s: %s", net_remoteaddr(hub->net), msg);
  }

  g_strfreev(cmd.argv);
}

#undef is_adcs_proto
#undef is_adc_proto
#undef is_valid_proto


// If port = 0, 'from' is interpreted as a nick. Otherwise, from should be an IP address.
static void nmdc_search(hub_t *hub, char *from, unsigned short port, int size_m, guint64 size, int type, char *query) {
  int max = port ? 10 : 5;
  fl_list_t *res[max];
  fl_search_t s = {};
  s.filedir = type == 1 ? 3 : type == 8 ? 2 : 1;
  s.ext = search_types[type].exts;
  s.size = size;
  s.sizem = size_m;
  int i = 0;

  // TTH lookup (YAY! this is fast!)
  if(type == 9) {
    if(strncmp(query, "TTH:", 4) != 0 || !istth(query+4)) {
      g_message("Invalid TTH $Search for %s", from);
      return;
    }
    char root[24];
    base32_decode(query+4, root);
    GSList *l = fl_local_from_tth(root);
    // it still has to match the other requirements...
    for(; i<max && l; l=l->next) {
      fl_list_t *c = l->data;
      if(fl_search_match_full(c, &s))
        res[i++] = c;
    }

  // Advanced lookup (Noo! This is slooow!)
  } else {
    char *tmp = query;
    for(; *tmp; tmp++)
      if(*tmp == '$')
        *tmp = ' ';
    tmp = nmdc_unescape_and_decode(hub, query);
    char **args = g_strsplit(tmp, " ", 0);
    g_free(tmp);
    s.and = fl_search_create_and(args);
    g_strfreev(args);
    i = fl_search_rec(fl_local_list, &s, res, max);
    fl_search_free_and(s.and);
  }

  // reply
  if(!i)
    return;

  const char *hubaddr = net_remoteaddr(hub->net);
  int slots = var_get_int(0, VAR_slots);
  int slots_free = slots - cc_slots_in_use(NULL);
  if(slots_free < 0)
    slots_free = 0;
  char tth[44] = "TTH:";
  tth[43] = 0;

  net_udp_t udp;
  if(port)
    net_udp_init(&udp, from, port, var_get(hub->id, VAR_local_address));

  while(--i>=0) {
    char *fl = fl_list_path(res[i]);
    // Windows style path delimiters... why!?
    char *tmp = fl;
    char *size = NULL;
    for(; *tmp; tmp++)
      if(*tmp == '/')
        *tmp = '\\';
    tmp = nmdc_encode_and_escape(hub, fl);
    if(res[i]->isfile) {
      base32_encode(res[i]->tth, tth+4);
      size = g_strdup_printf("\05%"G_GUINT64_FORMAT, res[i]->size);
    }
    char *msg = g_strdup_printf("$SR %s %s%s %d/%d\05%s (%s)",
      hub->nick_hub, tmp, size ? size : "", slots_free, slots, res[i]->isfile ? tth : hub->hubname_hub, hubaddr);
    if(!port)
      net_writef(hub->net, "%s\05%s|", msg, from);
    else
      net_udp_sendf(&udp, "%s|", msg);
    g_free(fl);
    g_free(msg);
    g_free(size);
    g_free(tmp);
  }

  if(port)
    net_udp_destroy(&udp);
}


static void nmdc_handle(net_t *net, char *cmd, int _len) {
  hub_t *hub = net_handle(net);
  // Immediately queue next read. It will be cancelled when net_disconnect() is
  // called anyway.
  net_readmsg(net, '|', nmdc_handle);

  if(!cmd[0])
    return;

  GMatchInfo *nfo;

  // create regexes (declared statically, allocated/compiled on first call)
#define CMDREGEX(name, regex) \
  static GRegex * name = NULL;\
  if(!name) name = g_regex_new("\\$" regex, G_REGEX_OPTIMIZE|G_REGEX_ANCHORED|G_REGEX_DOTALL|G_REGEX_RAW, 0, NULL)

  CMDREGEX(lock, "Lock ([^ $]+) Pk=[^ $]+");
  CMDREGEX(supports, "Supports (.+)");
  CMDREGEX(hello, "Hello ([^ $]+)");
  CMDREGEX(quit, "Quit ([^ $]+)");
  CMDREGEX(nicklist, "NickList (.+)");
  CMDREGEX(oplist, "OpList (.+)");
  CMDREGEX(userip, "UserIP (.+)");
  CMDREGEX(myinfo, "MyINFO \\$ALL ([^ $]+) (.+)");
  CMDREGEX(hubname, "HubName (.+)");
  CMDREGEX(to, "To: ([^ $]+) From: ([^ $]+) \\$(.+)");
  CMDREGEX(forcemove, "ForceMove (.+)");
  CMDREGEX(connecttome, "ConnectToMe ([^ $]+) ([a-fA-F0-9:\\[\\]\\.]+)(S|)");
  CMDREGEX(revconnecttome, "RevConnectToMe ([^ $]+) ([^ $]+)");
  CMDREGEX(search, "Search (Hub:(?:[^ $]+)|(?:[a-fA-F0-9:\\[\\]\\.]+)) ([TF])\\?([TF])\\?([0-9]+)\\?([1-9])\\?(.+)");

  // $Lock
  if(g_regex_match(lock, cmd, 0, &nfo)) { // 1 = lock
    char *lock = g_match_info_fetch(nfo, 1);
    if(strncmp(lock, "EXTENDEDPROTOCOL", 16) == 0)
      net_writestr(hub->net, "$Supports NoGetINFO NoHello UserIP2|");
    char *key = nmdc_lock2key(lock);
    net_writef(hub->net, "$Key %s|", key);
    hub->nick = g_strdup(var_get(hub->id, VAR_nick));
    hub->nick_hub = charset_convert(hub, FALSE, hub->nick);
    uit_hub_setnick(hub->tab);
    net_writef(hub->net, "$ValidateNick %s|", hub->nick_hub);
    g_free(key);
    g_free(lock);
  }
  g_match_info_free(nfo);

  // $Supports
  if(g_regex_match(supports, cmd, 0, &nfo)) { // 1 = list
    char *list = g_match_info_fetch(nfo, 1);
    if(strstr(list, "NoGetINFO"))
      hub->supports_nogetinfo = TRUE;
    // we also support NoHello, but no need to check for that
    g_free(list);
  }
  g_match_info_free(nfo);

  // $Hello
  if(g_regex_match(hello, cmd, 0, &nfo)) { // 1 = nick
    char *nick = g_match_info_fetch(nfo, 1);
    if(strcmp(nick, hub->nick_hub) == 0) {
      // some hubs send our $Hello twice (like verlihub)
      // just ignore the second one
      if(!hub->nick_valid) {
        hub->nick_valid = TRUE;
        ui_m(hub->tab, 0, "Nick validated.");
        net_writestr(hub->net, "$Version 1,0091|");
        hub_send_nfo(hub);
        net_writestr(hub->net, "$GetNickList|");
        // Most hubs send the user list after our nick has been validated (in
        // contrast to ADC), but it doesn't hurt to call this function at this
        // point anyway.
        dl_user_join(0);
      }
    } else {
      hub_user_t *u = user_add(hub, nick, NULL);
      if(!u->hasinfo && !hub->supports_nogetinfo)
        net_writef(hub->net, "$GetINFO %s|", nick);
    }
    g_free(nick);
  }
  g_match_info_free(nfo);

  // $Quit
  if(g_regex_match(quit, cmd, 0, &nfo)) { // 1 = nick
    char *nick = g_match_info_fetch(nfo, 1);
    hub_user_t *u = g_hash_table_lookup(hub->users, nick);
    if(u) {
      uit_hub_userchange(hub->tab, UIHUB_UC_QUIT, u);
      if(u->hasinfo) {
        hub->sharecount--;
        hub->sharesize -= u->sharesize;
      }
      g_hash_table_remove(hub->users, nick);
    }
    g_free(nick);
  }
  g_match_info_free(nfo);

  // $NickList
  if(g_regex_match(nicklist, cmd, 0, &nfo)) { // 1 = list of users
    // not really efficient, but does the trick
    char *str = g_match_info_fetch(nfo, 1);
    char **list = g_strsplit(str, "$$", 0);
    g_free(str);
    char **cur;
    for(cur=list; *cur&&**cur; cur++) {
      hub_user_t *u = user_add(hub, *cur, NULL);
      if(!u->hasinfo && !hub->supports_nogetinfo)
        net_writef(hub->net, "$GetINFO %s %s|", *cur, hub->nick_hub);
    }
    hub->received_first = TRUE;
    g_strfreev(list);
  }
  g_match_info_free(nfo);

  // $OpList
  if(g_regex_match(oplist, cmd, 0, &nfo)) { // 1 = list of ops
    // not really efficient, but does the trick
    char *str = g_match_info_fetch(nfo, 1);
    char **list = g_strsplit(str, "$$", 0);
    g_free(str);
    char **cur;
    // Actually, we should be going through the entire user list and set
    // isop=FALSE when the user is not listed here. I consider this to be too
    // inefficient and not all that important at this point.
    hub->isop = FALSE;
    for(cur=list; *cur&&**cur; cur++) {
      hub_user_t *u = user_add(hub, *cur, NULL);
      if(!u->isop) {
        u->isop = TRUE;
        uit_hub_userchange(hub->tab, UIHUB_UC_NFO, u);
      } else
        u->isop = TRUE;
      if(strcmp(hub->nick_hub, *cur) == 0)
        hub->isop = TRUE;
    }
    hub->received_first = TRUE;
    g_strfreev(list);
  }
  g_match_info_free(nfo);

  // $UserIP
  if(g_regex_match(userip, cmd, 0, &nfo)) { // 1 = list of users/ips
    char *str = g_match_info_fetch(nfo, 1);
    char **list = g_strsplit(str, "$$", 0);
    g_free(str);
    char **cur;
    for(cur=list; *cur&&**cur; cur++) {
      char *sep = strchr(*cur, ' ');
      if(!sep)
        continue;
      *sep = 0;
      hub_user_t *u = user_add(hub, *cur, NULL);
      if(ip4_isvalid(sep+1)) {
        struct in_addr new = ip4_pack(sep+1);
        if(ip4_cmp(new, u->ip4) != 0) {
          u->ip4 = new;
          uit_hub_userchange(hub->tab, UIHUB_UC_NFO, u);
        }
      } else {
        struct in6_addr new = ip6_pack(sep+1);
        if(ip6_cmp(new, u->ip6) != 0) {
          u->ip6 = new;
          uit_hub_userchange(hub->tab, UIHUB_UC_NFO, u);
        }
      }
      // Our own IP, configure active mode
      if(strcmp(*cur, hub->nick_hub) == 0)
        setownip(hub, u);
    }
    g_strfreev(list);
  }
  g_match_info_free(nfo);

  // $MyINFO
  if(g_regex_match(myinfo, cmd, 0, &nfo)) { // 1 = nick, 2 = info string
    char *nick = g_match_info_fetch(nfo, 1);
    char *str = g_match_info_fetch(nfo, 2);
    hub_user_t *u = user_add(hub, nick, NULL);
    if(!u->hasinfo)
      hub->sharecount++;
    else
      hub->sharesize -= u->sharesize;
    user_nmdc_nfo(hub, u, str);
    if(!u->hasinfo)
      hub->sharecount--;
    else
      hub->sharesize += u->sharesize;
    if(hub->received_first && !hub->joincomplete && hub->sharecount == g_hash_table_size(hub->users))
      hub->joincomplete = TRUE;
    g_free(str);
    g_free(nick);
  }
  g_match_info_free(nfo);

  // $HubName
  if(g_regex_match(hubname, cmd, 0, &nfo)) { // 1 = name
    g_free(hub->hubname_hub);
    g_free(hub->hubname);
    hub->hubname_hub = g_match_info_fetch(nfo, 1);
    hub->hubname = nmdc_unescape_and_decode(hub, hub->hubname_hub);
  }
  g_match_info_free(nfo);

  // $To
  if(g_regex_match(to, cmd, 0, &nfo)) { // 1 = to, 2 = from, 3 = msg
    char *to = g_match_info_fetch(nfo, 1);
    char *from = g_match_info_fetch(nfo, 2);
    char *msg = g_match_info_fetch(nfo, 3);
    char *msge = nmdc_unescape_and_decode(hub, msg);
    hub_user_t *u = g_hash_table_lookup(hub->users, from);
    if(!u) {
      g_message("[hub: %s] Got a $To from `%s', who is not on this hub!", hub->tab->name, from);
      char *user = charset_convert(hub, TRUE, from);
      ui_m(hub->tab, UIM_PASS|UIM_CHAT|UIP_MED, g_strdup_printf("<hub> PM from unknown user: <%s> %s", user, msge));
      free(user);
    } else
      uit_msg_msg(u, msge);
    g_free(msge);
    g_free(from);
    g_free(to);
    g_free(msg);
  }
  g_match_info_free(nfo);

  // $ForceMove
  if(g_regex_match(forcemove, cmd, 0, &nfo)) { // 1 = addr
    char *addr = g_match_info_fetch(nfo, 1);
    char *eaddr = nmdc_unescape_and_decode(hub, addr);
    ui_mf(hub->tab, UIP_HIGH, "\nThe hub is requesting you to move to %s.\nType `/connect %s' to do so.\n", eaddr, eaddr);
    hub_disconnect(hub, FALSE);
    g_free(eaddr);
    g_free(addr);
  }
  g_match_info_free(nfo);

  // $ConnectToMe
  if(g_regex_match(connecttome, cmd, 0, &nfo)) { // 1 = me, 2 = addr, 3 = TLS
    char *me = g_match_info_fetch(nfo, 1);
    char *addr = g_match_info_fetch(nfo, 2);
    char *tls = g_match_info_fetch(nfo, 3);
    if(strcmp(me, hub->nick_hub) != 0)
      g_message("Received a $ConnectToMe for someone else (to %s from %s)", me, addr);
    else {
      yuri_t uri;
      if(yuri_parse(addr, &uri) != 0 || *uri.scheme || uri.port == 0 ||
          uri.hosttype == YURI_DOMAIN || *uri.path || *uri.query || *uri.fragment)
        g_message("Invalid host:port in $ConnectToMe (%s)", addr);
      else
        cc_nmdc_connect(cc_create(hub), uri.host, uri.port, var_get(hub->id, VAR_local_address), *tls ? TRUE : FALSE);
    }
    g_free(me);
    g_free(addr);
    g_free(tls);
  }
  g_match_info_free(nfo);

  // $RevConnectToMe
  if(g_regex_match(revconnecttome, cmd, 0, &nfo)) { // 1 = other, 2 = me
    char *other = g_match_info_fetch(nfo, 1);
    char *me = g_match_info_fetch(nfo, 2);
    hub_user_t *u = g_hash_table_lookup(hub->users, other);
    if(strcmp(me, hub->nick_hub) != 0)
      g_message("Received a $RevConnectToMe for someone else (to %s from %s)", me, other);
    else if(!u)
      g_message("Received a $RevConnectToMe from someone not on the hub.");
    else if(listen_hub_active(hub->id)) {
      // Unlike with ADC, the client sending the $RCTM can not indicate it
      // wants to use TLS or not, so the decision is with us. Let's require
      // tls_policy to be PREFER here.
      int usetls = u->hastls && var_get_int(hub->id, VAR_tls_policy) == VAR_TLSP_PREFER;
      int port = listen_hub_tcp(hub->id);
      net_writef(hub->net, net_is_ipv6(hub->net) ? "$ConnectToMe %s [%s]:%d%s|" : "$ConnectToMe %s %s:%d%s|",
          other, hub_ip(hub), port, usetls ? "S" : "");
      cc_expect_add(hub, u, port, NULL, FALSE);
    } else
      g_message("Received a $RevConnectToMe, but we're not active.");
    g_free(me);
    g_free(other);
  }
  g_match_info_free(nfo);

  // $Search
  if(g_regex_match(search, cmd, 0, &nfo)) { // 1=from, 2=sizerestrict, 3=ismax, 4=size, 5=type, 6=query
    char *from = g_match_info_fetch(nfo, 1);
    char *sizerestrict = g_match_info_fetch(nfo, 2);
    char *ismax = g_match_info_fetch(nfo, 3);
    char *size = g_match_info_fetch(nfo, 4);
    char *type = g_match_info_fetch(nfo, 5);
    char *query = g_match_info_fetch(nfo, 6);
    unsigned short port = 0;
    char *nfrom = NULL;
    if(strncmp(from, "Hub:", 4) == 0) {
      if(strcmp(from+4, hub->nick_hub) != 0) /* Not our nick */
        nfrom = from+4;
    } else {
      yuri_t uri;
      if(yuri_parse(from, &uri) != 0 || *uri.scheme || uri.port == 0 ||
          uri.hosttype == YURI_DOMAIN || *uri.path || *uri.query || *uri.fragment)
        g_message("Invalid host:port in $Search (%s)", from);
      else if(!listen_hub_active(hub->id) || strcmp(uri.host, hub_ip(hub)) != 0 || uri.port != listen_hub_udp(hub->id)) {
        /* This search is not for our IP:port */
        nfrom = uri.host;
        port = uri.port;
      }
    }
    if(nfrom)
      nmdc_search(hub, nfrom, port, sizerestrict[0] == 'F' ? -2 : ismax[0] == 'T' ? -1 : 1, g_ascii_strtoull(size, NULL, 10), type[0]-'0', query);
    g_free(from);
    g_free(sizerestrict);
    g_free(ismax);
    g_free(size);
    g_free(type);
    g_free(query);
  }
  g_match_info_free(nfo);

  // $GetPass
  if(strncmp(cmd, "$GetPass", 8) == 0)
    hub_password(hub, NULL);

  // $BadPass
  if(strncmp(cmd, "$BadPass", 8) == 0) {
    if(var_get(hub->id, VAR_password))
      ui_m(hub->tab, 0, "Wrong password. Use '/hset password <password>' to edit your password or '/hunset password' to reset it.");
    else
      ui_m(hub->tab, 0, "Wrong password. Type /reconnect to try again.");
    hub_disconnect(hub, FALSE);
  }

  // $ValidateDenide
  if(strncmp(cmd, "$ValidateDenide", 15) == 0) {
    ui_m(hub->tab, 0, "Username invalid or already taken.");
    hub_disconnect(hub, TRUE);
  }

  // $HubIsFull
  if(strncmp(cmd, "$HubIsFull", 10) == 0) {
    ui_m(hub->tab, 0, "Hub is full.");
    hub_disconnect(hub, TRUE);
  }

  // $SR
  if(strncmp(cmd, "$SR", 3) == 0) {
    if(!search_handle_nmdc(hub, cmd))
      g_message("Received invalid $SR from %s", net_remoteaddr(hub->net));
  }

  // global hub message
  if(cmd[0] != '$') {
    char *msg = nmdc_unescape_and_decode(hub, cmd);
    if(msg[0] == '<' || (msg[0] == '*' && msg[1] == '*'))
      ui_m(hub->tab, UIM_PASS|UIM_CHAT|UIP_MED, msg);
    else {
      ui_m(hub->tab, UIM_PASS|UIM_CHAT|UIP_MED, g_strconcat("<hub> ", msg, NULL));
      g_free(msg);
    }
  }
}


static gboolean check_nfo(gpointer data) {
  hub_t *hub = data;
  if(hub->nick_valid)
    hub_send_nfo(hub);
  return TRUE;
}


static gboolean reconnect_timer(gpointer dat) {
  hub_connect(dat);
  ((hub_t *)dat)->reconnect_timer = 0;
  return FALSE;
}


static gboolean joincomplete_timer(gpointer dat) {
  hub_t *hub = dat;
  hub->joincomplete = TRUE;
  hub->joincomplete_timer = 0;
  return FALSE;
}


static void handle_error(net_t *n, int action, const char *err) {
  hub_t *hub = net_handle(n);

  ui_mf(hub->tab, 0, "%s: %s",
    action == NETERR_CONN ? "Could not connect to hub" :
    action == NETERR_RECV ? "Read error" : "Write error", err);

  hub_disconnect(hub, TRUE);
}


hub_t *hub_create(ui_tab_t *tab) {
  hub_t *hub = g_new0(hub_t, 1);

  // Get or create the hub id
  hub->id = db_vars_hubid(tab->name);
  if(!hub->id) {
    crypt_rnd(&hub->id, 8);
    var_set(hub->id, VAR_hubname, tab->name, NULL);
  }

  hub->net = net_new(hub, handle_error);
  hub->tab = tab;
  hub->users = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, user_free);
  hub->sessions = g_hash_table_new(g_direct_hash, g_direct_equal);
  hub->nfo_timer = g_timeout_add_seconds(60, check_nfo, hub);

  g_hash_table_insert(hubs, &hub->id, hub);
  return hub;
}

static void handle_fully_connected(hub_t *hub) {
  net_set_keepalive(hub->net, hub->adc ? "\n" : "|");

  /* If we have a pre-configured active IP, make sure to enable active mode
   * immediately. */
  if(hub_ip(hub))
    listen_refresh();

  if(hub->adc)
    net_writef(hub->net, "HSUP ADBASE ADTIGR%s\n", var_get_bool(hub->id, VAR_adc_blom) ? " ADBLO0 ADBLOM" : "");

  // In the case that the joincomplete detection fails, consider the join to be
  // complete anyway after a 2-minute timeout.
  hub->joincomplete_timer = g_timeout_add_seconds(120, joincomplete_timer, hub);

  // Start handling incoming messages
  net_readmsg(hub->net, hub->adc ? '\n' : '|', hub->adc ? adc_handle : nmdc_handle);
}

static void handle_handshake(net_t *n, const char *kpr, int proto) {
  g_return_if_fail(kpr != NULL);
  hub_t *hub = net_handle(n);
  g_return_if_fail(!hub->kp);

  if(proto == ALPN_NMDC) {
    hub->adc = FALSE;
    ui_mf(hub->tab, 0, "ALPN: negotiated NMDC.");
  } else if(proto == ALPN_ADC) {
    hub->adc = TRUE;
    ui_mf(hub->tab, 0, "ALPN: negotiated ADC.");
  }

  char kpf[53] = {};
  base32_encode_dat(kpr, kpf, 32);

  // Get configured keyprint
  char *old = var_get(hub->id, VAR_hubkp);

  // No keyprint? Then assume first-use trust and save it to the config file.
  if(!old) {
    ui_mf(hub->tab, 0, "No previous TLS keyprint known. Storing `%s' for future validation.", kpf);
    var_set(hub->id, VAR_hubkp, kpf, NULL);
    handle_fully_connected(hub);
    return;
  }

  // Keyprint matches? no problems!
  if(strcmp(old, kpf) == 0) {
    handle_fully_connected(hub);
    return;
  }

  // Keyprint doesn't match... now we have a problem!
  ui_mf(hub->tab, UIP_HIGH,
    "\nWARNING: The TLS certificate of this hub has changed!\n"
    "Old keyprint: %s\n"
    "New keyprint: %s\n"
    "This can mean two things:\n"
    "- The hub you are connecting to is NOT the same as the one you intended to connect to.\n"
    "- The hub owner has changed the TLS certificate.\n"
    "If you accept the new keyprint and wish continue connecting, type `/accept'.\n",
    old, kpf);
  hub_disconnect(hub, FALSE);
  hub->kp = g_slice_alloc(32);
  memcpy(hub->kp, kpr, 32);
}


static void handle_connect(net_t *n, const char *addr) {
  hub_t *hub = net_handle(n);
  if(addr) {
    ui_mf(hub->tab, 0, "Trying %s...", addr);
    return;
  }

  ui_mf(hub->tab, 0, "Connected to %s.", net_remoteaddr(n));

  if(hub->tls)
    net_settls(hub->net, FALSE, TRUE, handle_handshake);
  if(!net_is_connected(hub->net))
    return;

  // TLS may negotiate a different protocol, and handle_handshake
  // will eventually call handle_fully_connected as well.
  if(hub->tls)
    return;

  handle_fully_connected(hub);
}


void hub_connect(hub_t *hub) {
  char *oaddr = var_get(hub->id, VAR_hubaddr);
  yuri_t addr;
  yuri_parse_copy(oaddr, &addr);
  if(!addr.port)
    addr.port = 411;
  hub->adc = strncmp(addr.scheme, "adc", 3) == 0;
  hub->tls = strcmp(addr.scheme, "adcs") == 0 || strcmp(addr.scheme, "nmdcs") == 0;

  if(hub->reconnect_timer) {
    g_source_remove(hub->reconnect_timer);
    hub->reconnect_timer = 0;
  }
  if(hub->joincomplete_timer) {
    g_source_remove(hub->joincomplete_timer);
    hub->joincomplete_timer = 0;
  }

  ui_mf(hub->tab, 0, "Connecting to %s...", oaddr);
  net_connect(hub->net, addr.host, addr.port, var_get(hub->id, VAR_local_address), handle_connect);
  free(addr.buf);
}


void hub_disconnect(hub_t *hub, gboolean recon) {
  if(hub->reconnect_timer) {
    g_source_remove(hub->reconnect_timer);
    hub->reconnect_timer = 0;
  }
  if(hub->joincomplete_timer) {
    g_source_remove(hub->joincomplete_timer);
    hub->joincomplete_timer = 0;
  }
  net_disconnect(hub->net);
  if(hub->kp) {
    g_slice_free1(32, hub->kp);
    hub->kp = NULL;
  }
  uit_hub_disconnect(hub->tab);
  g_hash_table_remove_all(hub->sessions);
  g_hash_table_remove_all(hub->users);
  g_free(hub->nick);     hub->nick = NULL;
  g_free(hub->nick_hub); hub->nick_hub = NULL;
  g_free(hub->hubname);  hub->hubname = NULL;
  g_free(hub->hubname_hub);  hub->hubname_hub = NULL;
  g_free(hub->ip);       hub->ip = NULL;
  hub->nick_valid = hub->isreg = hub->isop = hub->received_first =
    hub->joincomplete =  hub->sharecount = hub->sharesize =
    hub->supports_nogetinfo = hub->state =
    hub->nfo_h_norm = hub->nfo_h_reg = hub->nfo_h_op = 0;
  if(!recon)
    ui_m(hub->tab, 0, "Disconnected.");
  else {
    int timeout = var_get_int(hub->id, VAR_reconnect_timeout);
    if(timeout) {
      ui_mf(hub->tab, 0, "Connection lost. Waiting %s before reconnecting.", str_formatinterval(timeout));
      hub->reconnect_timer = g_timeout_add_seconds(timeout, reconnect_timer, hub);
    } else
      ui_m(hub->tab, 0, "Connection lost.");
  }
}


void hub_free(hub_t *hub) {
  // Make sure to disconnect before calling cc_remove_hub(). dl_queue_expect(),
  // called from cc_remove_hub() will look in the global userlist for
  // alternative hubs. Users of this hub must not be present in the list,
  // otherwise things will go wrong.
  hub_disconnect(hub, FALSE);
  cc_remove_hub(hub);
  g_hash_table_remove(hubs, &hub->id);
  listen_refresh();

  net_unref(hub->net);
  g_free(hub->nfo_desc);
  g_free(hub->nfo_conn);
  g_free(hub->nfo_mail);
  g_free(hub->nfo_ip);
  g_free(hub->gpa_salt);
  g_hash_table_unref(hub->users);
  g_hash_table_unref(hub->sessions);
  g_source_remove(hub->nfo_timer);
  g_free(hub);
}

