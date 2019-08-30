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
#include "vars.h"



// Internal (low-level) utility functions

#define bool_raw(v) (!v ? FALSE : strcmp(v, "true") == 0 ? TRUE : FALSE)
#define int_raw(v) (!v ? 0 : g_ascii_strtoll(v, NULL, 0))


static gboolean bool_parse(const char *val, GError **err) {
  if(strcmp(val, "1") == 0 || strcmp(val, "t") == 0 || strcmp(val, "y") == 0
      || strcmp(val, "true") == 0 || strcmp(val, "yes") == 0 || strcmp(val, "on") == 0)
    return TRUE;
  if(strcmp(val, "0") == 0 || strcmp(val, "f") == 0 || strcmp(val, "n") == 0
      || strcmp(val, "false") == 0 || strcmp(val, "no") == 0 || strcmp(val, "off") == 0)
    return FALSE;
  g_set_error_literal(err, 1, 0, "Unrecognized boolean value.");
  return FALSE;
}

static char *f_id(const char *val) {
  return g_strdup(val);
}

#define f_bool f_id
#define f_int f_id

static char *f_interval(const char *val) {
  return g_strdup(str_formatinterval(int_raw(val)));
}

static char *f_speed(const char *val) {
  return g_strdup_printf("%s/s", str_formatsize(int_raw(val)));
}

static char *p_id(const char *val, GError **err) {
  return g_strdup(val);
}


static char *p_bool(const char *val, GError **err) {
  GError *e = NULL;
  gboolean b = bool_parse(val, &e);
  if(e) {
    g_propagate_error(err, e);
    return NULL;
  }
  return g_strdup(b ? "true" : "false");
}

static char *p_int(const char *val, GError **err) {
  long v = strtol(val, NULL, 10);
  if((!v && errno == EINVAL) || v < INT_MIN || v > INT_MAX || v < 0) {
    g_set_error_literal(err, 1, 0, "Invalid number.");
    return NULL;
  }
  return g_strdup_printf("%d", (int)v);
}

static char *p_int_ge1(const char *val, GError **err) {
  char *r = p_int(val, err);
  if(r && int_raw(r) < 1) {
    g_set_error_literal(err, 1, 0, "Invalid value.");
    g_free(r);
    return NULL;
  }
  return r;
}

static char *p_int_range(const char *val, int min, int max, const char *msg, GError **err) {
  char *r = p_int(val, err);
  if(r && (int_raw(r) < min || int_raw(r) > max)) {
    g_set_error_literal(err, 1, 0, msg);
    g_free(r);
    return NULL;
  }
  return r;
}

static char *p_interval(const char *val, GError **err) {
  int n = str_parseinterval(val);
  if(n < 0) {
    g_set_error_literal(err, 1, 0, "Invalid interval.");
    return NULL;
  }
  return g_strdup_printf("%d", n);
}

static char *p_ip(const char *val, GError **err) {
  struct in_addr i4 = ip4_any;
  struct in6_addr i6 = ip6_any;
  char *sep = strchr(val, ',');
  if(sep) *sep = 0;
  if(ip4_isvalid(val))
    i4 = ip4_pack(val);
  else if(ip6_isvalid(val))
    i6 = ip6_pack(val);
  else {
    g_set_error_literal(err, 1, 0, "Invalid IP.");
    return NULL;
  }
  if(sep) {
    *(sep++) = ',';
    while(*sep == ' ')
      sep++;
    if(ip4_isany(i4) && ip4_isvalid(sep))
      i4 = ip4_pack(sep);
    else if(ip6_isany(i6) && ip6_isvalid(sep))
      i6 = ip6_pack(sep);
    else {
      g_set_error_literal(err, 1, 0, "Invalid IP.");
      return NULL;
    }
  }
  return g_strdup_printf("%s,%s", ip4_unpack(i4), ip6_unpack(i6));
}

static char *p_active_ip(const char *val, GError **err) {
  if(strcmp(val, "local") == 0)
    return g_strdup(val);
  return p_ip(val, err);
}

static char *p_regex(const char *val, GError **err) {
  GRegex *r = g_regex_new(val, 0, 0, err);
  if(!r)
    return NULL;
  else {
    g_regex_unref(r);
    return g_strdup(val);
  }
}

static char *p_speed(const char *val, GError **err) {
  char *v = strlen(val) > 3 && strcmp(val+strlen(val)-2, "/s") == 0 ? g_strndup(val, strlen(val)-2) : g_strdup(val);
  guint64 size = str_parsesize(v);
  g_free(v);
  if(size == G_MAXUINT64) {
    g_set_error_literal(err, 1, 0, "Invalid speed.");
    return NULL;
  }
  return g_strdup_printf("%"G_GUINT64_FORMAT, size);
}

// Only suggests "true" or "false" regardless of the input. There are only two
// states anyway, and one would want to switch between those two without any
// hassle.
static void su_bool(const char *old, const char *val, char **sug) {
  gboolean f = !(val[0] == 0 || val[0] == '1' || val[0] == 't' || val[0] == 'y' || val[0] == 'o');
  sug[f ? 1 : 0] = g_strdup("true");
  sug[f ? 0 : 1] = g_strdup("false");
}


static void su_old(const char *old, const char *val, char **sug) {
  if(old && strncmp(old, val, strlen(val)) == 0)
    sug[0] = g_strdup(old);
}

static void su_path(const char *old, const char *val, char **sug) {
  path_suggest(val, sug);
}

static gboolean s_hubinfo(guint64 hub, const char *key, const char *val, GError **err) {
  db_vars_set(hub, key, val);
  hub_global_nfochange();
  return TRUE;
}

static gboolean s_active_conf(guint64 hub, const char *key, const char *val, GError **err) {
  db_vars_set(hub, key, val);
  listen_refresh();
  hub_global_nfochange();
  return TRUE;
}


typedef struct flag_option_t {
  int num;
  char *name;
} flag_option_t;

static int flags_raw(flag_option_t *ops, gboolean multi, const char *val, GError **err) {
  char **args = g_strsplit(val, ",", 0);
  int r = 0, n = 0;
  char **arg = args;
  for(; arg && *arg; arg++) {
    g_strstrip(*arg);
    if(!**arg)
      continue;

    flag_option_t *o = ops;
    for(; o->num; o++) {
      if(strcmp(o->name, *arg) == 0) {
        n++;
        r |= o->num;
        break;
      }
    }
    if(!o->num) {
      g_set_error(err, 1, 0, "Unknown flag: %s", *arg);
      g_strfreev(args);
      return 0;
    }
  }
  g_strfreev(args);
  if((!multi && n > 1) || n < 1) {
    g_set_error_literal(err, 1, 0, n > 1 ? "Too many flags." : "Not enough flags given.");
    return 0;
  }
  return r;
}

static char *flags_fmt(flag_option_t *o, int val) {
  GString *s = g_string_new("");
  for(; o->num; o++) {
    if(val & o->num) {
      if(s->str[0])
        g_string_append_c(s, ',');
      g_string_append(s, o->name);
    }
  }
  return g_string_free(s, FALSE);
}

static void flags_sug(flag_option_t *o, const char *val, char **sug) {
  char *v = g_strdup(val);
  char *attr = strrchr(v, ',');
  if(attr)
    *(attr++) = 0;
  else
    attr = v;
  g_strstrip(attr);
  int i = 0, len = strlen(attr);
  for(; o->num && i<20; o++)
    if(strncmp(attr, o->name, len) == 0)
      sug[i++] = g_strdup(o->name);
  if(i && attr != v)
    strv_prefix(sug, v, ",", NULL);
  g_free(v);
}



// active_*port

static char *p_active_port(const char *val, GError **err) {
  return p_int_range(val, 0, 65535, "Port number must be between 1 and 65535.", err);
}


static char *g_active_udp(guint64 hub, const char *key) {
  char *r = db_vars_get(hub, key);
  return r ? r : db_vars_get(hub, "active_port");
}


// autorefresh

static char *f_autorefresh(const char *val) {
  int n = int_raw(val);
  if(!n)
    return g_strconcat(str_formatinterval(n), " (disabled)", NULL);
  return f_interval(val);
}

static char *p_autorefresh(const char *val, GError **err) {
  char *raw = p_interval(val, err);
  if(raw && raw[0] != '0' && int_raw(raw) < 600) {
    g_set_error_literal(err, 1, 0, "Interval between automatic refreshes should be at least 10 minutes.");
    g_free(raw);
    return NULL;
  }
  return raw;
}


// backlog

static char *f_backlog(const char *var) {
  return g_strdup(strcmp(var, "0") == 0 ? "0 (disabled)" : var);
}

static char *p_backlog(const char *val, GError **err) {
  return p_int_range(val, 0, LOGWIN_BUF-1, "Maximum value is "G_STRINGIFY(LOGWIN_BUF-1)".", err);
}


// download_segment

static char *f_download_segment(const char *var) {
  return g_strdup(strcmp(var, "0") == 0 ? "0 (disable segmented downloading)" : str_formatsize(int_raw(var)));
}

static char *p_download_segment(const char *val, GError **err) {
  guint64 size = str_parsesize(val);
  if(size == G_MAXUINT64) {
    g_set_error_literal(err, 1, 0, "Invalid speed.");
    return NULL;
  }
  if(size && size < DLFILE_CHUNKSIZE)
    size = DLFILE_CHUNKSIZE;
  return g_strdup_printf("%"G_GUINT64_FORMAT, size);
}


// nick

static char *p_nick(const char *val, GError **err) {
  if(strlen(val) > 32) {
    g_set_error_literal(err, 1, 0, "Too long nick name.");
    return NULL;
  }
  if(strlen(val) < 1) {
    g_set_error_literal(err, 1, 0, "Too short nick name.");
    return NULL;
  }

  int i;
  for(i=strlen(val)-1; i>=0; i--)
    if(val[i] == '$' || val[i] == '|' || val[i] == ' ' || val[i] == '<' || val[i] == '>')
      break;
  if(i >= 0) {
    g_set_error_literal(err, 1, 0, "Invalid character in nick name.");
    return NULL;
  }

  ui_m(NULL, 0, "Your new nick will be used for new hub connections.");
  return g_strdup(val);
}

static gboolean s_nick(guint64 hub, const char *key, const char *val, GError **err) {
  // TODO: nick change without reconnect on ADC?
  if(!val && !hub) {
    g_set_error_literal(err, 1, 0, "May not be unset.");
    return FALSE;
  }
  db_vars_set(hub, key, val);
  return TRUE;
}

static char *i_nick() {
  // make sure a nick is set
  if(!db_vars_get(0, "nick")) {
    char *nick = g_strdup_printf("ncdc_%d", g_random_int_range(1, 9999));
    db_vars_set(0, "nick", nick);
    g_free(nick);
  }
  return "ncdc";
}


// cid / pid

static char *i_cid_pid() {
  if(db_vars_get(0, "cid") && db_vars_get(0, "pid"))
    return NULL;

  // Generate a random PID
  char pid[24];
  crypt_rnd(pid, 24);

  // now hash the PID so we have our CID
  char cid[24];
  tiger_ctx_t t;
  tiger_init(&t);
  tiger_update(&t, pid, 24);
  tiger_final(&t, cid);

  // encode and save
  char enc[40] = {};
  base32_encode(pid, enc);
  db_vars_set(0, "pid", enc);
  base32_encode(cid, enc);
  db_vars_set(0, "cid", enc);

  return NULL;
}


// color_*

static char *p_color(const char *val, GError **err) {
  short fg, bg;
  int x;
  if(!ui_color_str_parse(val, &fg, &bg, &x, err))
    return NULL;
  return g_strdup(ui_color_str_gen(fg, bg, x));
}

static void su_color(const char *old, const char *v, char **sug) {
  // TODO: use flags_sug()?
  char *val = g_strdup(v);
  char *attr = strrchr(val, ',');
  if(attr)
    *(attr++) = 0;
  else
    attr = val;
  g_strstrip(attr);
  ui_attr_t *a = ui_attr_names;
  int i = 0, len = strlen(attr);
  for(; a->name[0] && i<20; a++)
    if(strncmp(attr, a->name, len) == 0)
      sug[i++] = g_strdup(a->name);
  if(i && attr != val)
    strv_prefix(sug, val, ",", NULL);
  g_free(val);
}

static gboolean s_color(guint64 hub, const char *key, const char *val, GError **err) {
  db_vars_set(hub, key, val);
  ui_colors_update();
  return TRUE;
}


// download_dir & incoming_dir

static char *i_dl_inc_dir(gboolean dl) {
  return g_build_filename(db_dir, dl ? "dl" : "inc", NULL);
}

static gboolean s_dl_inc_dir(guint64 hub, const char *key, const char *val, GError **err) {
  gboolean dl = strcmp(key, "download_dir") == 0 ? TRUE : FALSE;

  // Don't allow changes to incoming_dir when the download queue isn't empty
  if(!dl && g_hash_table_size(dl_queue) > 0) {
    g_set_error_literal(err, 1, 0, "Can't change the incoming directory unless the download queue is empty.");
    return FALSE;
  }

  char *tmp = val ? g_strdup(val) : i_dl_inc_dir(dl);
  char nval[strlen(tmp)+1];
  strcpy(nval, tmp);
  g_free(tmp);

  // make sure it exists
  if(g_mkdir_with_parents(nval, 0777)) {
    g_set_error(err, 1, 0, "Error creating the directory: %s", g_strerror(errno));
    return FALSE;
  }

  // test whether they are on the same filesystem
  struct stat a, b;
  char *bd = var_get(0, dl ? VAR_incoming_dir : VAR_download_dir);
  if(stat(bd, &b) == 0) {
    if(stat(nval, &a) < 0) {
      g_set_error(err, 1, 0, "Error stat'ing %s: %s", nval, g_strerror(errno));
      return FALSE;
    }
    if(a.st_dev != b.st_dev)
      ui_m(NULL, 0, "WARNING: The download directory is not on the same filesystem as the incoming"
                    " directory. This may cause the program to hang when downloading large files.");
  }

  db_vars_set(hub, key, val);
  return TRUE;
}


// download_slots

static gboolean s_download_slots(guint64 hub, const char *key, const char *val, GError **err) {
  int old = var_get_int(hub, VAR_download_slots);
  db_vars_set(hub, key, val);
  if(int_raw(val) > old)
    dl_queue_start();
  return TRUE;
}


// encoding

static char *p_encoding(const char *val, GError **err) {
  if(!str_convert_check(val, err)) {
    if(err && !*err)
      g_set_error_literal(err, 1, 0, "Invalid encoding.");
    return NULL;
  }
  return g_strdup(val);
}

static void su_encoding(const char *old, const char *val, char **sug) {
  static flag_option_t encoding_flags[] = {
    {1,"CP1250"}, {1,"CP1251"}, {1,"CP1252"}, {1,"ISO-2022-JP"}, {1,"ISO-8859-2"}, {1,"ISO-8859-7"},
    {1,"ISO-8859-8"}, {1,"ISO-8859-9"}, {1,"KOI8-R"}, {1,"LATIN1"}, {1,"SJIS"}, {1,"UTF-8"},
    {1,"WINDOWS-1250"}, {1,"WINDOWS-1251"}, {1,"WINDOWS-1252"}, {0}
  };
  flags_sug(encoding_flags, val, sug);
}


// email / description / connection

static char *p_connection(const char *val, GError **err) {
  if(!str_connection_to_speed(val))
    ui_mf(NULL, 0, "Couldn't convert `%s' to bytes/second, won't broadcast upload speed on ADC. See `/help set connection' for more information.", val);
  return g_strdup(val);
}


// flush_file_cache

// Special interface to allow quick and threaded access to the current value
#if INTERFACE
#define var_ffc_get() g_atomic_int_get(&var_ffc)
#define var_ffc_set(v) g_atomic_int_set(&var_ffc, v)
#endif

int var_ffc = 0;

#if INTERFACE
#define VAR_FFC_NONE     1
#define VAR_FFC_DOWNLOAD 2
#define VAR_FFC_UPLOAD   4
#define VAR_FFC_HASH     8
#endif

static flag_option_t var_ffc_ops[] = {
  { VAR_FFC_NONE,     "none"     },
  { VAR_FFC_DOWNLOAD, "download" },
  { VAR_FFC_UPLOAD,   "upload"   },
  { VAR_FFC_HASH,     "hash"     },
  { 0 }
};

static char *f_ffc(const char *raw) {
#if HAVE_POSIX_FADVISE
  return flags_fmt(var_ffc_ops, int_raw(raw));
#else
  return g_strdup("none (not supported)");
#endif
}

static char *p_ffc(const char *val, GError **err) {
  int n = flags_raw(var_ffc_ops, TRUE, val, err);
  if(n & VAR_FFC_NONE)
    n = VAR_FFC_NONE;
  return n ? g_strdup_printf("%d", n) : NULL;
}

static void su_ffc(const char *old, const char *val, char **sug) {
  flags_sug(var_ffc_ops, val, sug);
}

static char *g_ffc(guint64 hub, const char *key) {
#ifndef HAVE_POSIX_FADVISE
  return G_STRINGIFY(VAR_FFC_NONE);
#else
  char *r = db_vars_get(hub, key);
  if(!r)
    return NULL;
  static char num[4];
  // true/false check is for compatibility with old versions
  g_snprintf(num, 4, "%d",
       strcmp(r, "true") == 0 ? VAR_FFC_UPLOAD | VAR_FFC_HASH
    : strcmp(r, "false") == 0 ? VAR_FFC_NONE
                              : flags_raw(var_ffc_ops, TRUE, r, NULL)
  );
  return num;
#endif
}

static gboolean s_ffc(guint64 hub, const char *key, const char *val, GError **err) {
#ifndef HAVE_POSIX_FADVISE
  g_set_error(err, 1, 0, "This option can't be modified: %s.", "posix_fadvise() not supported");
  return FALSE;
#else
  char *r = flags_fmt(var_ffc_ops, int_raw(val));
  db_vars_set(hub, key, r[0] ? r : NULL);
  g_free(r);
  var_ffc_set(int_raw(val));
  return TRUE;
#endif
}

static char *i_ffc() {
  char *r = g_ffc(0, "flush_file_cache");
  var_ffc_set(int_raw(r));
  return G_STRINGIFY(VAR_FFC_NONE);
}


// geoip

static gboolean s_geoip_cc(guint64 hub, const char *key, const char *val, GError **err) {
#ifdef USE_GEOIP
  db_vars_set(hub, key, val);
  geoip_reinit();
  return TRUE;
#else
  g_set_error(err, 1, 0, "This option can't be modified: %s.", "Ncdc has not been compiled with GeoIP support");
  return FALSE;
#endif
}


// hubname

static char *p_hubname(const char *val, GError **err) {
  if(val[0] == '#')
    val++;
  char *g = g_strdup_printf("#%s", val);
  if(!str_is_valid_hubname(g+1)) {
    g_set_error_literal(err, 1, 0, "Illegal characters or too long.");
    g_free(g);
    return NULL;
  } else if(db_vars_hubid(g)) {
    g_set_error_literal(err, 1, 0, "Name already used.");
    g_free(g);
    return NULL;
  }
  return g;
}

static gboolean s_hubname(guint64 hub, const char *key, const char *val, GError **err) {
  if(!val) {
    g_set_error_literal(err, 1, 0, "May not be unset.");
    return FALSE;
  }
  db_vars_set(hub, key, val);
  GList *n;
  for(n=ui_tabs; n; n=n->next) {
    ui_tab_t *t = n->data;
    if(t->type == uit_hub && t->hub->id == hub) {
      g_free(t->name);
      t->name = g_strdup(val);
    }
  }
  return TRUE;
}


// log_debug

gboolean var_log_debug = TRUE;

static gboolean s_log_debug(guint64 hub, const char *key, const char *val, GError **err) {
  db_vars_set(hub, key, val);
  var_log_debug = bool_raw(val);
  return TRUE;
}

static char *i_log_debug() {
  char *r = db_vars_get(0, "log_debug");
  var_log_debug = bool_raw(r);
  return "false";
}


// minislot_size

static char *p_minislot_size(const char *val, GError **err) {
  char *r = p_int(val, err);
  int n = r ? int_raw(r) : 0;
  g_free(r);
  if(r && n < 64) {
    g_set_error_literal(err, 1, 0, "Minislot size must be at least 64 KiB.");
    return NULL;
  }
  return r ? g_strdup_printf("%d", MIN(G_MAXINT, n*1024)) : NULL;
}

static char *f_minislot_size(const char *val) {
  return g_strdup_printf("%d KiB", (int)int_raw(val)/1024);
}


// password

static char *f_password(const char *val) {
  char *r = g_malloc(strlen(val)+1);
  memset(r, '*', strlen(val));
  r[strlen(val)-1] = 0;
  return r;
}

static gboolean s_password(guint64 hub, const char *key, const char *val, GError **err) {
  db_vars_set(hub, key, val);
  // send password to hub
  hub_t *h = hub_global_byid(hub);
  if(h && net_is_connected(h->net) && !h->nick_valid)
    hub_password(h, NULL);
  return TRUE;
}


// sendfile

static char *f_sendfile(const char *val) {
#ifdef HAVE_SENDFILE
  return f_id(val);
#else
  return g_strdup("false (not supported)");
#endif
}

static char *p_sendfile(const char *val, GError **err) {
  char *r = p_bool(val, err);
#ifndef HAVE_SENDFILE
  if(r && bool_raw(val)) {
    g_set_error(err, 1, 0, "This option can't be modified: %s.", "sendfile() not supported");
    g_free(r);
    r = NULL;
  }
#endif
  return r;
}


// tls_policy

#if INTERFACE
#define VAR_TLSP_DISABLE 1
#define VAR_TLSP_ALLOW   2
#define VAR_TLSP_PREFER  4
#endif

static flag_option_t var_tls_policy_ops[] = {
  { VAR_TLSP_DISABLE, "disabled" },
  { VAR_TLSP_ALLOW,   "allow"    },
  { VAR_TLSP_PREFER,  "prefer"   },
  { 0 }
};

static char *f_tls_policy(const char *val) {
  return flags_fmt(var_tls_policy_ops, int_raw(val));
}

static char *p_tls_policy(const char *val, GError **err) {
  int n = flags_raw(var_tls_policy_ops, FALSE, val, err);
  return n ? g_strdup_printf("%d", n) : NULL;
}

static void su_tls_policy(const char *old, const char *val, char **sug) {
  flags_sug(var_tls_policy_ops, val, sug);
}

static char *g_tls_policy(guint64 hub, const char *key) {
  char *r = db_vars_get(hub, key);
  if(!r)
    return NULL;
  static char num[2] = {};
  // Compatibility with old versions
  if(r && r[0] >= '0' && r[0] <= '2' && !r[1])
    num[0] = var_tls_policy_ops[r[0]-'0'].num;
  else
    num[0] = flags_raw(var_tls_policy_ops, FALSE, r, NULL);
  num[0] += '0';
  return num;
}

static gboolean s_tls_policy(guint64 hub, const char *key, const char *val, GError **err) {
  char *r = flags_fmt(var_tls_policy_ops, int_raw(val));
  db_vars_set(hub, key, r[0] ? r : NULL);
  g_free(r);
  listen_refresh();
  hub_global_nfochange();
  return TRUE;
}


// tls_priority

static char *p_tls_priority(const char *val, GError **err) {
  gnutls_priority_t prio;
  const char *pos;
  if(gnutls_priority_init(&prio, val, &pos) != GNUTLS_E_SUCCESS) {
    g_set_error(err, 1, 0, "Error parsing priority string at '%s'", pos);
    return NULL;
  }
  gnutls_priority_deinit(prio);
  return g_strdup(val);
}


// notify_bell

#if INTERFACE
#define VAR_NOTB_DISABLE 1
#define VAR_NOTB_LOW     2
#define VAR_NOTB_MED     4
#define VAR_NOTB_HIGH    8
#endif

static flag_option_t var_notify_bell_ops[] = {
  { VAR_NOTB_DISABLE, "disabled" },
  { VAR_NOTB_LOW,     "low"      },
  { VAR_NOTB_MED,     "medium"   },
  { VAR_NOTB_HIGH,    "high"     },
  { 0 }
};

static char *f_notify_bell(const char *val) {
  return flags_fmt(var_notify_bell_ops, int_raw(val));
}

static char *p_notify_bell(const char *val, GError **err) {
  int n = flags_raw(var_notify_bell_ops, FALSE, val, err);
  return n ? g_strdup_printf("%d", n) : NULL;
}

static void su_notify_bell(const char *old, const char *val, char **sug) {
  flags_sug(var_notify_bell_ops, val, sug);
}

static char *g_notify_bell(guint64 hub, const char *key) {
  char *r = db_vars_get(hub, key);
  if(!r)
    return NULL;
  static char num[2] = {};
  num[0] = '0' + flags_raw(var_notify_bell_ops, FALSE, r, NULL);
  return num;
}

static gboolean s_notify_bell(guint64 hub, const char *key, const char *val, GError **err) {
  char *r = flags_fmt(var_notify_bell_ops, int_raw(val));
  db_vars_set(hub, key, r[0] ? r : NULL);
  g_free(r);
  return TRUE;
}


// sudp_policy


#if INTERFACE
#define VAR_SUDPP_DISABLE 1
#define VAR_SUDPP_ALLOW   2
#define VAR_SUDPP_PREFER  4
#endif

static flag_option_t var_sudp_policy_ops[] = {
  { VAR_SUDPP_DISABLE, "disabled" },
  { VAR_SUDPP_ALLOW,   "allow"    },
  { VAR_SUDPP_PREFER,  "prefer"   },
  { 0 }
};

static char *f_sudp_policy(const char *val) {
  return flags_fmt(var_sudp_policy_ops, int_raw(val));
}

static char *p_sudp_policy(const char *val, GError **err) {
  int n = flags_raw(var_sudp_policy_ops, FALSE, val, err);
  return n ? g_strdup_printf("%d", n) : NULL;
}

static void su_sudp_policy(const char *old, const char *val, char **sug) {
  flags_sug(var_sudp_policy_ops, val, sug);
}

static char *g_sudp_policy(guint64 hub, const char *key) {
  static char num[2] = {};
  char *r = db_vars_get(hub, key);
  if(!r)
    return NULL;
  num[0] = '0' + flags_raw(var_sudp_policy_ops, FALSE, r, NULL);
  return num;
}

static gboolean s_sudp_policy(guint64 hub, const char *key, const char *val, GError **err) {
  char *r = flags_fmt(var_sudp_policy_ops, int_raw(val));
  db_vars_set(hub, key, r[0] ? r : NULL);
  g_free(r);
  hub_global_nfochange();
  return TRUE;
}





// Exported data

#if INTERFACE

struct var_t {
  // Name does not necessarily have to correspond to the name in the 'vars'
  // table. Though in that case special getraw() and setraw() functions have to
  // be used.
  const char *name;
  gboolean global : 1;
  gboolean hub : 1;

  // Formats the raw value for human viewing. Returned string will be
  // g_free()'d. May be NULL if !hub && !global.
  char *(*format)(const char *val);

  // Validates and parses a human input string and returns the "raw" string.
  // Returned string will be g_free()'d. May also return an error if the
  // setting can't be set yet (e.g. if some other setting has to be set
  // first.). Will write any warnings or notes to ui_m(NULL, ..).
  // May be NULL if !hub && !global.
  char *(*parse)(const char *val, GError **err);

  // Suggestion function. *old is the old (raw) value. *val the current string
  // on the input line. May be NULL if no suggestions are available.
  void (*sug)(const char *old, const char *val, char **sug);

  // Get the raw value. The returned string will not be freed and may be
  // modified later. When this is NULL, db_vars_get() is used.
  char *(*getraw)(guint64 hub, const char *name);

  // Set the raw value and make sure it's active. val = NULL to unset it. In
  // general, this function should not fail if parse() didn't return an error,
  // but it may still refuse to set the value set *err to indicate failure.
  // (e.g. when trying to unset a var that must always exist).
  gboolean (*setraw)(guint64 hub, const char *name, const char *val, GError **err);

  // Default raw value, to be used when getraw() returns NULL.
  char *def;
};


// name               g h  format          parse            suggest        getraw        setraw           default/init
#define VARS\
  V(active,           1,1, f_bool,         p_bool,          su_bool,       NULL,         s_active_conf,   "false")\
  V(active_ip,        1,1, f_id,           p_active_ip,     su_old,        NULL,         s_active_conf,   NULL)\
  V(active_port,      1,1, f_int,          p_active_port,   NULL,          NULL,         s_active_conf,   NULL)\
  V(active_udp_port,  1,1, f_int,          p_active_port,   NULL,          g_active_udp, s_active_conf,   NULL)\
  V(adc_blom,         1,1, f_bool,         p_bool,          su_bool,       NULL,         NULL,            "false")\
  V(autoconnect,      0,1, f_bool,         p_bool,          su_bool,       NULL,         NULL,            "false")\
  V(autorefresh,      1,0, f_autorefresh,  p_autorefresh,   NULL,          NULL,         NULL,            "3600")\
  V(backlog,          1,1, f_backlog,      p_backlog,       NULL,          NULL,         NULL,            "0")\
  V(chat_only,        1,1, f_bool,         p_bool,          su_bool,       NULL,         NULL,            "false")\
  V(cid,              0,0, NULL,           NULL,            NULL,          NULL,         NULL,            i_cid_pid())\
  UI_COLORS \
  V(connection,       1,1, f_id,           p_connection,    su_old,        NULL,         s_hubinfo,       NULL)\
  V(description,      1,1, f_id,           p_id,            su_old,        NULL,         s_hubinfo,       NULL)\
  V(disconnect_offline,1,1,f_bool,         p_bool,          su_bool,       NULL,         NULL,            "false")\
  V(download_dir,     1,0, f_id,           p_id,            su_path,       NULL,         s_dl_inc_dir,    i_dl_inc_dir(TRUE))\
  V(download_exclude, 1,0, f_id,           p_regex,         su_old,        NULL,         NULL,            NULL)\
  V(download_rate,    1,0, f_speed,        p_speed,         NULL,          NULL,         NULL,            NULL)\
  V(download_segment, 1,0, f_download_segment,p_download_segment,NULL,     NULL,         NULL,            g_strdup_printf("%"G_GUINT64_FORMAT, (guint64)DLFILE_CHUNKSIZE))\
  V(download_shared,  1,0, f_bool,         p_bool,          su_bool,       NULL,         NULL,            "true")\
  V(download_slots,   1,0, f_int,          p_int,           NULL,          NULL,         s_download_slots,"3")\
  V(email,            1,1, f_id,           p_id,            su_old,        NULL,         s_hubinfo,       NULL)\
  V(encoding,         1,1, f_id,           p_encoding,      su_encoding,   NULL,         NULL,            "UTF-8")\
  V(filelist_maxage,  1,0, f_interval,     p_interval,      su_old,        NULL,         NULL,            "604800")\
  V(fl_done,          0,0, NULL,           NULL,            NULL,          NULL,         NULL,            "false")\
  V(flush_file_cache, 1,0, f_ffc,          p_ffc,           su_ffc,        g_ffc,        s_ffc,           i_ffc())\
  V(geoip_cc,         1,0, f_id,           p_id,            su_path,       NULL,         s_geoip_cc,      NULL)\
  V(hash_rate,        1,0, f_speed,        p_speed,         NULL,          NULL,         NULL,            NULL)\
  V(hubaddr,          0,0, NULL,           NULL,            NULL,          NULL,         NULL,            NULL)\
  V(hubkp,            0,0, NULL,           NULL,            NULL,          NULL,         NULL,            NULL)\
  V(hubname,          0,1, f_id,           p_hubname,       su_old,        NULL,         s_hubname,       NULL)\
  V(incoming_dir,     1,0, f_id,           p_id,            su_path,       NULL,         s_dl_inc_dir,    i_dl_inc_dir(FALSE))\
  V(local_address,    1,1, f_id,           p_ip,            su_old,        NULL,         s_active_conf,   db_vars_get(0, "active_bind"))\
  V(log_debug,        1,0, f_bool,         p_bool,          su_bool,       NULL,         s_log_debug,     i_log_debug())\
  V(log_downloads,    1,0, f_bool,         p_bool,          su_bool,       NULL,         NULL,            "true")\
  V(log_hubchat,      1,1, f_bool,         p_bool,          su_bool,       NULL,         NULL,            "true")\
  V(log_uploads,      1,0, f_bool,         p_bool,          su_bool,       NULL,         NULL,            "true")\
  V(max_ul_per_user,  1,1, f_int,          p_int_ge1,       NULL,          NULL,         NULL,            "1")\
  V(minislots,        1,0, f_int,          p_int_ge1,       NULL,          NULL,         NULL,            "3")\
  V(minislot_size,    1,0, f_minislot_size,p_minislot_size, NULL,          NULL,         NULL,            "65536")\
  V(nick,             1,1, f_id,           p_nick,          su_old,        NULL,         s_nick,          i_nick())\
  V(notify_bell,      1,0, f_notify_bell,  p_notify_bell,   su_notify_bell,g_notify_bell,s_notify_bell,   G_STRINGIFY(VAR_NOTB_DISABLE))\
  V(password,         0,1, f_password,     p_id,            NULL,          NULL,         s_password,      NULL)\
  V(pid,              0,0, NULL,           NULL,            NULL,          NULL,         NULL,            i_cid_pid())\
  V(reconnect_timeout,1,1, f_interval,     p_interval,      su_old,        NULL,         NULL,            "30")\
  V(sendfile,         1,0, f_sendfile,     p_sendfile,      su_bool,       NULL,         NULL,            "true")\
  V(share_emptydirs,  1,0, f_bool,         p_bool,          su_bool,       NULL,         NULL,            "false")\
  V(share_exclude,    1,0, f_id,           p_regex,         su_old,        NULL,         NULL,            NULL)\
  V(share_hidden,     1,0, f_bool,         p_bool,          su_bool,       NULL,         NULL,            "false")\
  V(share_symlinks,   1,0, f_bool,         p_bool,          su_bool,       NULL,         NULL,            "false")\
  V(show_free_slots,  1,1, f_bool,         p_bool,          su_bool,       NULL,         s_hubinfo,       "false")\
  V(show_joinquit,    1,1, f_bool,         p_bool,          su_bool,       NULL,         NULL,            "false")\
  V(slots,            1,0, f_int,          p_int_ge1,       NULL,          NULL,         s_hubinfo,       "10")\
  V(sudp_policy,      1,0, f_sudp_policy,  p_sudp_policy,   su_sudp_policy,g_sudp_policy,s_sudp_policy,   G_STRINGIFY(VAR_SUDPP_PREFER))\
  V(tls_policy,       1,1, f_tls_policy,   p_tls_policy,    su_tls_policy, g_tls_policy, s_tls_policy,    G_STRINGIFY(VAR_TLSP_PREFER))\
  V(tls_priority,     1,0, f_id,           p_tls_priority,  su_old,        NULL,         NULL,            "NORMAL:-ARCFOUR-128")\
  V(ui_time_format,   1,0, f_id,           p_id,            su_old,        NULL,         NULL,            "[%H:%M:%S]")\
  V(upload_rate,      1,0, f_speed,        p_speed,         NULL,          NULL,         NULL,            NULL)

enum var_type {
#define V(n, gl, h, f, p, su, g, s, d) VAR_##n,
#define C(n, d) VAR_color_##n,
  VARS
#undef V
#undef C
  VAR_END
};

#endif


var_t vars[] = {
#define V(n, gl, h, f, p, su, g, s, d) { G_STRINGIFY(n), gl, h, f, p, su, g, s, NULL },
#define C(n, d) { "color_"G_STRINGIFY(n), 1, 0, f_id, p_color, su_color, NULL, s_color, d },
  VARS
#undef V
#undef C
  { NULL }
};




// Exported functions


// Get a var id by name. Returns -1 if not found.
// TODO: case insensitive? Allow '-' in addition to '_'?
// TODO: binary search?
int vars_byname(const char *n) {
  int i;
  for(i=0; i<VAR_END; i++)
    if(strcmp(vars[i].name, n) == 0)
      break;
  return i==VAR_END ? -1 : i;
}


// Calls setraw() on the specified var
gboolean var_set(guint64 h, var_type n, const char *v, GError **err) {
  if(vars[n].setraw)
    return vars[n].setraw(h, vars[n].name, v, err);
  db_vars_set(h, vars[n].name, v);
  return FALSE;
}


gboolean var_set_bool(guint64 h, var_type n, gboolean v) {
  return var_set(h, n, v ? "true" : "false", NULL);
}


// Calls getraw() on the specified var. If h != 0 and no value is found for
// that hub, then another getraw() will be called with h = 0. If that fails,
// the default value is returned instead.
char *var_get(guint64 h, var_type n) {
  char *r = NULL;
  if(vars[n].getraw)
    r = vars[n].getraw(h, vars[n].name);
  else
    r = db_vars_get(h, vars[n].name);
  return r ? r : h ? var_get(0, n) : vars[n].def;
}


gboolean var_get_bool(guint64 h, var_type n) {
  char *r = var_get(h, n);
  return bool_raw(r);
}

int var_get_int(guint64 h, var_type n) {
  char *r = var_get(h, n);
  return int_raw(r);
}

gint64 var_get_int64(guint64 h, var_type n) {
  char *r = var_get(h, n);
  return int_raw(r);
}

// Extract the IPv4 address from a p_ip()-formatted config option.
struct in_addr var_parse_ip4(char *r) {
  struct in_addr a = ip4_any;
  if(!r)
    return a;
  char *sep = strchr(r, ',');
  if(sep) *sep = 0;
  a = ip4_pack(r);
  if(sep) *sep = ',';
  if(sep && ip4_isany(a)) {
    sep++;
    while(*sep == ' ')
      sep++;
    a = ip4_pack(sep);
  }
  return a;
}

// Likewise, for IPv6
struct in6_addr var_parse_ip6(char *r) {
  struct in6_addr a = ip6_any;
  if(!r)
    return a;
  char *sep = strchr(r, ',');
  if(sep) *sep = 0;
  a = ip6_pack(r);
  if(sep) *sep = ',';
  if(sep && ip6_isany(a)) {
    sep++;
    while(*sep == ' ')
      sep++;
    a = ip6_pack(sep);
  }
  return a;
}




// Initialization

void vars_init() {
  // Set correct default value
  int var_num = 0;
#define V(n, gl, h, f, p, su, g, s, d)\
    { vars[var_num].def = d; };\
    var_num++;
  // Colors already have their defaults initialized statically
#define C(n, d) var_num++;
  VARS
#undef C
#undef V
}

