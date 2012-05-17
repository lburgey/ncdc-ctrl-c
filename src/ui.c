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
#include "ui.h"
#include <math.h>


#if INTERFACE

// These are assumed to occupy two bits
#define UIP_EMPTY 0 // no change
#define UIP_LOW   1 // system messages
#define UIP_MED   2 // chat messages, or error messages in the main tab
#define UIP_HIGH  3 // direct messages to you (PM or name mentioned)

struct ui_tab_type_t {
  void (*draw)(ui_tab_t *);
  char *(*title)(ui_tab_t *);
  void (*key)(ui_tab_t *, guint64);
  void (*close)(ui_tab_t *);
};

struct ui_tab_t {
  ui_tab_type_t *type;
  int prio;               // UIP_ type
  char *name;
  ui_tab_t *parent;       // the tab that opened this tab (may be NULL or dangling)
  ui_logwindow_t *log;    // HUB, MSG
  hub_t *hub;             // HUB, USERLIST, MSG
  ui_listing_t *list;     // USERLIST
  guint64 uid;            // MSG
  int order : 4;          // USERLIST
  gboolean o_reverse : 1; // USERLIST
  gboolean details : 1;   // USERLIST
  // USERLIST
  gboolean user_opfirst : 1;
  gboolean user_hide_desc : 1;
  gboolean user_hide_tag : 1;
  gboolean user_hide_mail : 1;
  gboolean user_hide_conn : 1;
  gboolean user_hide_ip : 1;
  // HUB
  gboolean hub_joincomplete : 1;
  GRegex *hub_highlight;
  char *hub_nick;
  ui_tab_t *userlist_tab;
  // MSG
  int msg_replyto;
};

#endif

GList *ui_tabs = NULL;
GList *ui_tab_cur = NULL;

// screen dimensions
int wincols;
int winrows;

gboolean ui_beep = FALSE; // set to true anywhere to send a beep

// uid -> tab lookup table for MSG tabs.
GHashTable *ui_msg_tabs = NULL;




// User message tab

ui_tab_type_t uit_msg[1];

ui_tab_t *ui_msg_create(hub_t *hub, hub_user_t *user) {
  g_return_val_if_fail(!g_hash_table_lookup(ui_msg_tabs, &user->uid), NULL);

  ui_tab_t *tab = g_new0(ui_tab_t, 1);
  tab->type = uit_msg;
  tab->hub = hub;
  tab->uid = user->uid;
  tab->name = g_strdup_printf("~%s", user->name);
  tab->log = ui_logwindow_create(tab->name, var_get_int(0, VAR_backlog));
  tab->log->handle = tab;
  tab->log->checkchat = ui_hub_log_checkchat;

  ui_mf(tab, 0, "Chatting with %s on %s.", user->name, hub->tab->name);
  g_hash_table_insert(ui_msg_tabs, &tab->uid, tab);

  return tab;
}


static void ui_msg_close(ui_tab_t *tab) {
  g_hash_table_remove(ui_msg_tabs, &tab->uid);
  ui_tab_remove(tab);
  ui_logwindow_free(tab->log);
  g_free(tab->name);
  g_free(tab);
}


// *u may be NULL if change = QUIT. A QUIT is always done before the user is
// removed from the hub_uids table.
static void ui_msg_userchange(ui_tab_t *tab, int change, hub_user_t *u) {
  switch(change) {
  case UIHUB_UC_JOIN:
    ui_mf(tab, 0, "--> %s has joined.", u->name);
    break;
  case UIHUB_UC_QUIT:
    if(g_hash_table_lookup(hub_uids, &tab->uid))
      ui_mf(tab, 0, "--< %s has quit.", tab->name+1);
    break;
  case UIHUB_UC_NFO:
    // Detect nick changes.
    // Note: the name of the log file remains the same even after a nick
    // change. This probably isn't a major problem, though. Nick changes are
    // not very common and are only detected on ADC hubs.
    if(strcmp(u->name, tab->name+1) != 0) {
      ui_mf(tab, 0, "%s is now known as %s.", tab->name+1, u->name);
      g_free(tab->name);
      tab->name = g_strdup_printf("~%s", u->name);
    }
    break;
  }
}


static void ui_msg_draw(ui_tab_t *tab) {
  ui_logwindow_draw(tab->log, 1, 0, winrows-4, wincols);

  mvaddstr(winrows-3, 0, tab->name);
  addstr("> ");
  int pos = str_columns(tab->name)+2;
  ui_textinput_draw(ui_global_textinput, winrows-3, pos, wincols-pos);
}


static char *ui_msg_title(ui_tab_t *tab) {
  return g_strdup_printf("Chatting with %s on %s%s.",
    tab->name+1, tab->hub->tab->name, g_hash_table_lookup(hub_uids, &tab->uid) ? "" : " (offline)");
}


static void ui_msg_key(ui_tab_t *tab, guint64 key) {
  char *str = NULL;
  if(!ui_logwindow_key(tab->log, key, winrows) &&
      ui_textinput_key(ui_global_textinput, key, &str) && str) {
    cmd_handle(str);
    g_free(str);
  }
}


static void ui_msg_msg(ui_tab_t *tab, const char *msg, int replyto) {
  ui_m(tab, UIP_HIGH, msg);
  tab->msg_replyto = replyto;
}


ui_tab_type_t uit_msg[1] = { { ui_msg_draw, ui_msg_title, ui_msg_key, ui_msg_close } };





// Hub tab

ui_tab_type_t uit_hub[1];

#if INTERFACE
// change types for ui_hub_userchange()
#define UIHUB_UC_JOIN 0
#define UIHUB_UC_QUIT 1
#define UIHUB_UC_NFO 2
#endif


// Also used for ui_msg_*
int ui_hub_log_checkchat(void *dat, char *nick, char *msg) {
  ui_tab_t *tab = dat;
  tab = tab->hub->tab;
  if(!tab->hub_nick)
    return 0;

  if(strcmp(nick, tab->hub_nick) == 0)
    return 2;

  if(!tab->hub_highlight)
    return 0;

  return g_regex_match(tab->hub_highlight, msg, 0, NULL) ? 1 : 0;
}


// Called by hub.c when hub->nick is set or changed. (Not called when hub->nick is reset to NULL)
// A local hub_nick field is kept in the hub tab struct to still provide
// highlighting for it after disconnecting from the hub.
void ui_hub_setnick(ui_tab_t *tab) {
  if(!tab->hub->nick)
    return;
  g_free(tab->hub_nick);
  if(tab->hub_highlight)
    g_regex_unref(tab->hub_highlight);
  tab->hub_nick = g_strdup(tab->hub->nick);
  char *name = g_regex_escape_string(tab->hub->nick, -1);
  char *pattern = g_strdup_printf("\\b%s\\b", name);
  tab->hub_highlight = g_regex_new(pattern, G_REGEX_CASELESS|G_REGEX_OPTIMIZE, 0, NULL);
  g_free(name);
  g_free(pattern);
}


ui_tab_t *ui_hub_create(const char *name, gboolean conn) {
  ui_tab_t *tab = g_new0(ui_tab_t, 1);
  // NOTE: tab name is also used as configuration group
  tab->name = g_strdup_printf("#%s", name);
  tab->type = uit_hub;
  tab->hub = hub_create(tab);
  tab->log = ui_logwindow_create(tab->name, var_get_int(tab->hub->id, VAR_backlog));
  tab->log->handle = tab;
  tab->log->checkchat = ui_hub_log_checkchat;
  // already used this name before? open connection again
  if(conn && var_get(tab->hub->id, VAR_hubaddr))
    hub_connect(tab->hub);
  return tab;
}


static void ui_hub_close(ui_tab_t *tab) {
  // close the userlist tab
  if(tab->userlist_tab)
    ui_userlist_close(tab->userlist_tab);
  // close msg and search tabs
  GList *n;
  for(n=ui_tabs; n;) {
    ui_tab_t *t = n->data;
    n = n->next;
    if(t->type == uit_msg && t->hub == tab->hub)
      ui_msg_close(t);
  }
  // remove ourself from the list
  ui_tab_remove(tab);

  hub_free(tab->hub);
  ui_logwindow_free(tab->log);
  g_free(tab->hub_nick);
  if(tab->hub_highlight)
    g_regex_unref(tab->hub_highlight);
  g_free(tab->name);
  g_free(tab);
}


static void ui_hub_draw(ui_tab_t *tab) {
  ui_logwindow_draw(tab->log, 1, 0, winrows-5, wincols);

  attron(UIC(separator));
  mvhline(winrows-4, 0, ' ', wincols);
  if(net_is_connecting(tab->hub->net))
    mvaddstr(winrows-4, wincols-15, "Connecting...");
  else if(!net_is_connected(tab->hub->net))
    mvaddstr(winrows-4, wincols-16, "Not connected.");
  else if(!tab->hub->nick_valid)
    mvaddstr(winrows-4, wincols-15, "Logging in...");
  else {
    char *addr = var_get(tab->hub->id, VAR_hubaddr);
    char *conn = !listen_hub_active(tab->hub->id) ? g_strdup("[passive]")
      : g_strdup_printf("[active: %s]", ip4_unpack(hub_ip4(tab->hub)));
    char *tmp = g_strdup_printf("%s @ %s%s %s", tab->hub->nick, addr,
      tab->hub->isop ? " (operator)" : tab->hub->isreg ? " (registered)" : "", conn);
    g_free(conn);
    mvaddstr(winrows-4, 0, tmp);
    g_free(tmp);
    int count = g_hash_table_size(tab->hub->users);
    tmp = g_strdup_printf("%6d users  %10s%c", count,
      str_formatsize(tab->hub->sharesize), tab->hub->sharecount == count ? ' ' : '+');
    mvaddstr(winrows-4, wincols-26, tmp);
    g_free(tmp);
  }
  attroff(UIC(separator));

  mvaddstr(winrows-3, 0, tab->name);
  addstr("> ");
  int pos = str_columns(tab->name)+2;
  ui_textinput_draw(ui_global_textinput, winrows-3, pos, wincols-pos);
}


static char *ui_hub_title(ui_tab_t *tab) {
  return g_strdup_printf("%s: %s", tab->name,
    net_is_connecting(tab->hub->net) ? "Connecting..." :
    !net_is_connected(tab->hub->net) ? "Not connected." :
    !tab->hub->nick_valid            ? "Logging in..." :
    tab->hub->hubname                ? tab->hub->hubname : "Connected.");
}


static void ui_hub_key(ui_tab_t *tab, guint64 key) {
  char *str = NULL;
  if(!ui_logwindow_key(tab->log, key, winrows) &&
      ui_textinput_key(ui_global_textinput, key, &str) && str) {
    cmd_handle(str);
    g_free(str);
  } else if(key == INPT_ALT('u'))
    ui_hub_userlist_open(tab);
}


void ui_hub_userchange(ui_tab_t *tab, int change, hub_user_t *user) {
  // notify the userlist, when it is open
  if(tab->userlist_tab)
    ui_userlist_userchange(tab->userlist_tab, change, user);

  // notify the MSG tab, if we have one open for this user
  ui_tab_t *mt = g_hash_table_lookup(ui_msg_tabs, &user->uid);
  if(mt)
    ui_msg_userchange(mt, change, user);

  // display the join/quit, when requested
  gboolean log = var_get_bool(tab->hub->id, VAR_show_joinquit);
  if(change == UIHUB_UC_NFO && !user->isjoined) {
    user->isjoined = TRUE;
    if(log && tab->hub->joincomplete && (!tab->hub->nick_valid
        || (tab->hub->adc ? (tab->hub->sid != user->sid) : (strcmp(tab->hub->nick_hub, user->name_hub) != 0))))
      ui_mf(tab, 0, "--> %s has joined.", user->name);
  } else if(change == UIHUB_UC_QUIT && log)
    ui_mf(tab, 0, "--< %s has quit.", user->name);
}


// Called when the hub is disconnected. Notifies any msg tabs and the userlist
// tab, if there's one open.
void ui_hub_disconnect(ui_tab_t *tab) {
  // userlist
  if(tab->userlist_tab)
    ui_userlist_disconnect(tab->userlist_tab);
  // msg tabs
  GList *n = ui_tabs;
  for(; n; n=n->next) {
    ui_tab_t *t = n->data;
    if(t->type == uit_msg && t->hub == tab->hub)
      ui_msg_userchange(t, UIHUB_UC_QUIT, NULL);
  }
}


void ui_hub_msg(ui_tab_t *tab, hub_user_t *user, const char *msg, int replyto) {
  ui_tab_t *t = g_hash_table_lookup(ui_msg_tabs, &user->uid);
  if(!t) {
    t = ui_msg_create(tab->hub, user);
    ui_tab_open(t, FALSE, tab);
  }
  ui_msg_msg(t, msg, replyto);
}


void ui_hub_userlist_open(ui_tab_t *tab) {
  if(tab->userlist_tab)
    ui_tab_cur = g_list_find(ui_tabs, tab->userlist_tab);
  else {
    tab->userlist_tab = ui_userlist_create(tab->hub);
    ui_tab_open(tab->userlist_tab, TRUE, tab);
  }
}


gboolean ui_hub_finduser(ui_tab_t *tab, guint64 uid, const char *user, gboolean utf8) {
  hub_user_t *u =
    uid ? g_hash_table_lookup(hub_uids, &uid) :
    utf8 ? hub_user_get(tab->hub, user) : g_hash_table_lookup(tab->hub->users, user);
  if(!u || u->hub != tab->hub)
    return FALSE;
  ui_hub_userlist_open(tab);
  // u->iter should be valid at this point.
  tab->userlist_tab->list->sel = u->iter;
  tab->userlist_tab->details = TRUE;
  return TRUE;
}


ui_tab_type_t uit_hub[1] = { { ui_hub_draw, ui_hub_title, ui_hub_key, ui_hub_close } };





// Userlist tab

ui_tab_type_t uit_userlist[1];

// Columns to sort on
#define UIUL_USER   0
#define UIUL_SHARE  1
#define UIUL_CONN   2
#define UIUL_DESC   3
#define UIUL_MAIL   4
#define UIUL_CLIENT 5
#define UIUL_IP     6


static gint ui_userlist_sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const hub_user_t *a = da;
  const hub_user_t *b = db;
  ui_tab_t *tab = dat;
  int p = tab->order;

  if(tab->user_opfirst && !a->isop != !b->isop)
    return a->isop && !b->isop ? -1 : 1;

  // All orders have the username as secondary order.
  int o = p == UIUL_USER ? 0 :
    p == UIUL_SHARE  ? a->sharesize > b->sharesize ? 1 : -1:
    p == UIUL_CONN   ? (tab->hub->adc ? a->conn - b->conn : strcmp(a->conn?a->conn:"", b->conn?b->conn:"")) :
    p == UIUL_DESC   ? g_utf8_collate(a->desc?a->desc:"", b->desc?b->desc:"") :
    p == UIUL_MAIL   ? g_utf8_collate(a->mail?a->mail:"", b->mail?b->mail:"") :
    p == UIUL_CLIENT ? strcmp(a->client?a->client:"", b->client?b->client:"")
                     : ip4_cmp(a->ip4, b->ip4);

  // Username sort
  if(!o)
    o = g_utf8_collate(a->name, b->name);
  if(!o && a->name_hub && b->name_hub)
    o = strcmp(a->name_hub, b->name_hub);
  if(!o)
    o = a - b;
  return tab->o_reverse ? -1*o : o;
}


ui_tab_t *ui_userlist_create(hub_t *hub) {
  ui_tab_t *tab = g_new0(ui_tab_t, 1);
  tab->name = g_strdup_printf("@%s", hub->tab->name+1);
  tab->type = uit_userlist;
  tab->hub = hub;
  tab->user_opfirst = TRUE;
  tab->user_hide_conn = TRUE;
  tab->user_hide_mail = TRUE;
  tab->user_hide_ip = TRUE;
  GSequence *users = g_sequence_new(NULL);
  // populate the list
  // g_sequence_sort() uses insertion sort? in that case it is faster to insert
  // all items using g_sequence_insert_sorted() rather than inserting them in
  // no particular order and then sorting them in one go. (which is faster for
  // linked lists, since it uses a faster sorting algorithm)
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, hub->users);
  hub_user_t *u;
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&u))
    u->iter = g_sequence_insert_sorted(users, u, ui_userlist_sort_func, tab);
  tab->list = ui_listing_create(users);
  return tab;
}


void ui_userlist_close(ui_tab_t *tab) {
  tab->hub->tab->userlist_tab = NULL;
  ui_tab_remove(tab);
  // To clean things up, we should also reset all hub_user->iter fields. But
  // this isn't all that necessary since they won't be used anymore until they
  // get reset in a subsequent ui_userlist_create().
  g_sequence_free(tab->list->list);
  ui_listing_free(tab->list);
  g_free(tab->name);
  g_free(tab);
}


static char *ui_userlist_title(ui_tab_t *tab) {
  return g_strdup_printf("%s / User list", tab->hub->tab->name);
}


#define DRAW_COL(row, colvar, width, str) do {\
    if(width > 1)\
      mvaddnstr(row, colvar, str, str_offset_from_columns(str, width-1));\
    colvar += width;\
  } while(0)


typedef struct ui_userlist_draw_opts_t {
  int cw_user, cw_share, cw_conn, cw_desc, cw_mail, cw_tag, cw_ip;
} ui_userlist_draw_opts_t;


static void ui_userlist_draw_row(ui_listing_t *list, GSequenceIter *iter, int row, void *dat) {
  hub_user_t *user = g_sequence_get(iter);
  ui_userlist_draw_opts_t *o = dat;

  char *tag = hub_user_tag(user);
  char *conn = hub_user_conn(user);
  int j=5;

  attron(iter == list->sel ? UIC(list_select) : UIC(list_default));
  mvhline(row, 0, ' ', wincols);
  if(iter == list->sel)
    mvaddstr(row, 0, ">");

  if(user->isop)
    mvaddch(row, 2, 'O');
  if(!user->active)
    mvaddch(row, 3, 'P');
  DRAW_COL(row, j, o->cw_user,  user->name);
  DRAW_COL(row, j, o->cw_share, user->hasinfo ? str_formatsize(user->sharesize) : "");
  DRAW_COL(row, j, o->cw_desc,  user->desc?user->desc:"");
  DRAW_COL(row, j, o->cw_tag,   tag?tag:"");
  DRAW_COL(row, j, o->cw_mail,  user->mail?user->mail:"");
  DRAW_COL(row, j, o->cw_conn,  conn?conn:"");
  DRAW_COL(row, j, o->cw_ip,    user->ip4?ip4_unpack(user->ip4):"");
  g_free(conn);
  g_free(tag);

  attroff(iter == list->sel ? UIC(list_select) : UIC(list_default));
}


/* Distributing a width among several columns with given weights:
 *   w_t = sum(i=c_v; w_i)
 *   w_s = 1 + sum(i=c_h; w_i/w_t)
 *   b_i = w_i*w_s
 * Where:
 *   c_v = set of all visible columns
 *   c_h = set of all hidden columns
 *   w_i = weight of column $i
 *   w_t = sum of the weights of all visible columns
 *   w_s = scale factor
 *   b_i = calculated width of column $i, with 0 < b_i <= 1
 *
 * TODO: abstract this, so that the weights and such don't need repetition.
 */
static void ui_userlist_calc_widths(ui_tab_t *tab, ui_userlist_draw_opts_t *o) {
  // available width
  int w = wincols-5;

  // share has a fixed size
  o->cw_share = 12;
  w -= 12;

  // IP column as well
  o->cw_ip = tab->user_hide_ip ? 0 : 16;
  w -= o->cw_ip;

  // User column has a minimum size (but may grow a bit later on, so will still be counted as a column)
  o->cw_user = 15;
  w -= 15;

  // Total weight (first one is for the user column)
  double wt = 0.02
    + (tab->user_hide_conn ? 0.0 : 0.16)
    + (tab->user_hide_desc ? 0.0 : 0.32)
    + (tab->user_hide_mail ? 0.0 : 0.18)
    + (tab->user_hide_tag  ? 0.0 : 0.32);

  // Scale factor
  double ws = 1.0 + (
    + (tab->user_hide_conn ? 0.16: 0.0)
    + (tab->user_hide_desc ? 0.32: 0.0)
    + (tab->user_hide_mail ? 0.18: 0.0)
    + (tab->user_hide_tag  ? 0.32: 0.0))/wt;
  // scale to available width
  ws *= w;

  // Get the column widths. Note the use of floor() here, this prevents that
  // the total width exceeds the available width. The remaining columns will be
  // given to the user column, which is always present anyway.
  o->cw_conn = tab->user_hide_conn ? 0 : floor(0.16*ws);
  o->cw_desc = tab->user_hide_desc ? 0 : floor(0.32*ws);
  o->cw_mail = tab->user_hide_mail ? 0 : floor(0.18*ws);
  o->cw_tag  = tab->user_hide_tag  ? 0 : floor(0.32*ws);
  o->cw_user += w - o->cw_conn - o->cw_desc - o->cw_mail - o->cw_tag;
}


static void ui_userlist_draw(ui_tab_t *tab) {
  ui_userlist_draw_opts_t o;
  ui_userlist_calc_widths(tab, &o);

  // header
  int i = 5;
  attron(UIC(list_header));
  mvhline(1, 0, ' ', wincols);
  mvaddstr(1, 2, "OP");
  DRAW_COL(1, i, o.cw_user,  "Username");
  DRAW_COL(1, i, o.cw_share, "Share");
  DRAW_COL(1, i, o.cw_desc,  "Description");
  DRAW_COL(1, i, o.cw_tag,   "Tag");
  DRAW_COL(1, i, o.cw_mail,  "E-Mail");
  DRAW_COL(1, i, o.cw_conn,  "Connection");
  DRAW_COL(1, i, o.cw_ip,    "IP");
  attroff(UIC(list_header));

  // rows
  int bottom = tab->details ? winrows-7 : winrows-3;
  int pos = ui_listing_draw(tab->list, 2, bottom-1, ui_userlist_draw_row, &o);

  // footer
  attron(UIC(separator));
  mvhline(bottom, 0, ' ', wincols);
  int count = g_hash_table_size(tab->hub->users);
  mvaddstr(bottom, 0, "Totals:");
  mvprintw(bottom, o.cw_user+5, "%s%c   %d users",
    str_formatsize(tab->hub->sharesize), tab->hub->sharecount == count ? ' ' : '+', count);
  mvprintw(bottom, wincols-6, "%3d%%", pos);
  attroff(UIC(separator));

  // detailed info box
  if(!tab->details)
    return;
  if(g_sequence_iter_is_end(tab->list->sel))
    mvaddstr(bottom+1, 2, "No user selected.");
  else {
    hub_user_t *u = g_sequence_get(tab->list->sel);
    attron(A_BOLD);
    mvaddstr(bottom+1,  4, "Username:");
    mvaddstr(bottom+1, 41, "Share:");
    mvaddstr(bottom+2,  2, "Connection:");
    mvaddstr(bottom+2, 40, "E-Mail:");
    mvaddstr(bottom+3, 10, "IP:");
    mvaddstr(bottom+3, 43, "Tag:");
    mvaddstr(bottom+4,  1, "Description:");
    attroff(A_BOLD);
    mvaddstr(bottom+1, 14, u->name);
    if(u->hasinfo)
      mvprintw(bottom+1, 48, "%s (%s bytes)", str_formatsize(u->sharesize), str_fullsize(u->sharesize));
    else
      mvaddstr(bottom+1, 48, "-");
    char *conn = hub_user_conn(u);
    mvaddstr(bottom+2, 14, conn?conn:"-");
    g_free(conn);
    mvaddstr(bottom+2, 48, u->mail?u->mail:"-");
    mvaddstr(bottom+3, 14, u->ip4?ip4_unpack(u->ip4):"-");
    char *tag = hub_user_tag(u);
    mvaddstr(bottom+3, 48, tag?tag:"-");
    g_free(tag);
    mvaddstr(bottom+4, 14, u->desc?u->desc:"-");
    // TODO: CID?
  }
}
#undef DRAW_COL


static void ui_userlist_key(ui_tab_t *tab, guint64 key) {
  if(ui_listing_key(tab->list, key, winrows/2))
    return;

  hub_user_t *sel = g_sequence_iter_is_end(tab->list->sel) ? NULL : g_sequence_get(tab->list->sel);
  gboolean sort = FALSE;
  switch(key) {
  case INPT_CHAR('?'):
    uit_main_keys("userlist");
    break;

  // Sorting
#define SETSORT(c) \
  tab->o_reverse = tab->order == c ? !tab->o_reverse : FALSE;\
  tab->order = c;\
  sort = TRUE;

  case INPT_CHAR('s'): // s/S - sort on share size
  case INPT_CHAR('S'):
    SETSORT(UIUL_SHARE);
    break;
  case INPT_CHAR('u'): // u/U - sort on username
  case INPT_CHAR('U'):
    SETSORT(UIUL_USER)
    break;
  case INPT_CHAR('D'): // D - sort on description
    SETSORT(UIUL_DESC)
    break;
  case INPT_CHAR('T'): // T - sort on client (= tag)
    SETSORT(UIUL_CLIENT)
    break;
  case INPT_CHAR('E'): // E - sort on email
    SETSORT(UIUL_MAIL)
    break;
  case INPT_CHAR('C'): // C - sort on connection
    SETSORT(UIUL_CONN)
    break;
  case INPT_CHAR('P'): // P - sort on IP
    SETSORT(UIUL_IP)
    break;
  case INPT_CHAR('o'): // o - toggle sorting OPs before others
    tab->user_opfirst = !tab->user_opfirst;
    sort = TRUE;
    break;
#undef SETSORT

  // Column visibility
  case INPT_CHAR('d'): // d (toggle description visibility)
    tab->user_hide_desc = !tab->user_hide_desc;
    break;
  case INPT_CHAR('t'): // t (toggle tag visibility)
    tab->user_hide_tag = !tab->user_hide_tag;
    break;
  case INPT_CHAR('e'): // e (toggle e-mail visibility)
    tab->user_hide_mail = !tab->user_hide_mail;
    break;
  case INPT_CHAR('c'): // c (toggle connection visibility)
    tab->user_hide_conn = !tab->user_hide_conn;
    break;
  case INPT_CHAR('p'): // p (toggle IP visibility)
    tab->user_hide_ip = !tab->user_hide_ip;
    break;

  case INPT_CTRL('j'): // newline
  case INPT_CHAR('i'): // i       (toggle user info)
    tab->details = !tab->details;
    break;
  case INPT_CHAR('m'): // m (/msg user)
    if(!sel)
      ui_m(NULL, 0, "No user selected.");
    else {
      ui_tab_t *t = g_hash_table_lookup(ui_msg_tabs, &sel->uid);
      if(!t) {
        t = ui_msg_create(tab->hub, sel);
        ui_tab_open(t, TRUE, tab);
      } else
        ui_tab_cur = g_list_find(ui_tabs, t);
    }
    break;
  case INPT_CHAR('g'): // g (grant slot)
    if(!sel)
      ui_m(NULL, 0, "No user selected.");
    else {
      cc_grant(sel);
      ui_m(NULL, 0, "Slot granted.");
    }
    break;
  case INPT_CHAR('b'): // b (/browse userlist)
  case INPT_CHAR('B'): // B (force /browse userlist)
    if(!sel)
      ui_m(NULL, 0, "No user selected.");
    else
      uit_fl_queue(sel->uid, key == INPT_CHAR('B'), NULL, tab, TRUE, FALSE);
    break;
  }

  // TODO: some way to save the column visibility? per hub? global default?

  if(sort) {
    g_sequence_sort(tab->list->list, ui_userlist_sort_func, tab);
    ui_listing_sorted(tab->list);
    ui_mf(NULL, 0, "Ordering by %s (%s%s)",
        tab->order == UIUL_USER  ? "user name" :
        tab->order == UIUL_SHARE ? "share size" :
        tab->order == UIUL_CONN  ? "connection" :
        tab->order == UIUL_DESC  ? "description" :
        tab->order == UIUL_MAIL  ? "e-mail" :
        tab->order == UIUL_CLIENT? "tag" : "IP address",
      tab->o_reverse ? "descending" : "ascending", tab->user_opfirst ? ", OPs first" : "");
  }
}


// Called when the hub is disconnected. All users should be removed in one go,
// this is faster than a _userchange() for every user.
void ui_userlist_disconnect(ui_tab_t *tab) {
  g_sequence_free(tab->list->list);
  ui_listing_free(tab->list);
  tab->list = ui_listing_create(g_sequence_new(NULL));
}


void ui_userlist_userchange(ui_tab_t *tab, int change, hub_user_t *user) {
  if(change == UIHUB_UC_JOIN) {
    user->iter = g_sequence_insert_sorted(tab->list->list, user, ui_userlist_sort_func, tab);
    ui_listing_inserted(tab->list);
  } else if(change == UIHUB_UC_QUIT) {
    g_return_if_fail(g_sequence_get(user->iter) == (gpointer)user);
    ui_listing_remove(tab->list, user->iter);
    g_sequence_remove(user->iter);
  } else {
    g_sequence_sort_changed(user->iter, ui_userlist_sort_func, tab);
    ui_listing_sorted(tab->list);
  }
}


ui_tab_type_t uit_userlist[1] = { { ui_userlist_draw, ui_userlist_title, ui_userlist_key, ui_userlist_close } };






// Generic message displaying thing.

#if INTERFACE

// These flags can be OR'ed together with UIP_ flags. No UIP_ flag or UIP_EMPTY
// implies UIP_LOW. There is no need to set any priority when tab == NULL,
// since it will be displayed right away anyway.

// Message should also be notified in status bar (implied automatically if the
// requested tab has no log window). This also uses UIP_EMPTY if no other UIP_
// flag is set.
#define UIM_NOTIFY  4
// Ownership of the message string is passed to the message handling function.
// (Which fill g_free() it after use)
#define UIM_PASS    8
// This is a chat message, i.e. check to see if your name is part of the
// message, and if so, give it UIP_HIGH.
#define UIM_CHAT   16
// Indicates that ui_m_mainthread() is called directly - without using an idle
// function.
#define UIM_DIRECT 32
// Do not log to the tab. Implies UIM_NOTIFY
#define UIM_NOLOG  (64 | UIM_NOTIFY)

#endif


static char *ui_m_text = NULL;
static guint ui_m_timer;
static gboolean ui_m_updated = FALSE;

typedef struct ui_m_t {
  char *msg;
  ui_tab_t *tab;
  int flags;
} ui_m_t;


static gboolean ui_m_timeout(gpointer data) {
  if(ui_m_text) {
    g_free(ui_m_text);
    ui_m_text = NULL;
    g_source_remove(ui_m_timer);
    ui_m_updated = TRUE;
  }
  return FALSE;
}


static gboolean ui_m_mainthread(gpointer dat) {
  ui_m_t *msg = dat;
  ui_tab_t *tab = msg->tab;
  int prio = msg->flags & 3; // lower two bits
  if(!tab)
    tab = ui_tab_cur->data;
  // It can happen that the tab is closed while we were waiting for this idle
  // function to be called, so check whether it's still in the list.
  else if(!(msg->flags & UIM_DIRECT) && !g_list_find(ui_tabs, tab))
    goto ui_m_cleanup;

  gboolean notify = (msg->flags & UIM_NOTIFY) || !tab->log;

  if(notify && ui_m_text) {
    g_free(ui_m_text);
    ui_m_text = NULL;
    g_source_remove(ui_m_timer);
    ui_m_updated = TRUE;
  }
  if(notify && msg->msg) {
    ui_m_text = g_strdup(msg->msg);
    ui_m_timer = g_timeout_add(3000, ui_m_timeout, NULL);
    ui_m_updated = TRUE;
  }
  if(tab->log && msg->msg && !(msg->flags & (UIM_NOLOG & ~UIM_NOTIFY))) {
    if((msg->flags & UIM_CHAT) && tab->type == uit_hub && tab->hub_highlight
        && g_regex_match(tab->hub_highlight, msg->msg, 0, NULL))
      prio = UIP_HIGH;
    ui_logwindow_add(tab->log, msg->msg);
    tab->prio = MAX(tab->prio, MAX(prio, notify ? UIP_EMPTY : UIP_LOW));
  }

ui_m_cleanup:
  g_free(msg->msg);
  g_free(msg);
  return FALSE;
}


// a notication message, either displayed in the log of the current tab or, if
// the hub has no tab, in the "status bar". Calling this function with NULL
// will reset the status bar message. Unlike everything else, this function can
// be called from any thread. (It will queue an idle function, after all)
void ui_m(ui_tab_t *tab, int flags, const char *msg) {
  ui_m_t *dat = g_new0(ui_m_t, 1);
  dat->msg = (flags & UIM_PASS) ? (char *)msg : g_strdup(msg);
  dat->tab = tab;
  dat->flags = flags;
  // call directly if we're running from the main thread. use an idle function
  // otherwise.
  if((dat->flags & UIM_DIRECT) || g_main_context_is_owner(NULL)) {
    dat->flags |= UIM_DIRECT;
    ui_m_mainthread(dat);
  } else
    g_idle_add_full(G_PRIORITY_HIGH_IDLE, ui_m_mainthread, dat, NULL);
}


// UIM_PASS shouldn't be used here (makes no sense).
void ui_mf(ui_tab_t *tab, int flags, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  ui_m(tab, flags | UIM_PASS, g_strdup_vprintf(fmt, va));
  va_end(va);
}





// Global stuff

ui_textinput_t *ui_global_textinput;

void ui_tab_open(ui_tab_t *tab, gboolean sel, ui_tab_t *parent) {
  ui_tabs = g_list_append(ui_tabs, tab);
  tab->parent = parent;
  if(sel)
    ui_tab_cur = g_list_last(ui_tabs);
}


// to be called from ui_*_close()
void ui_tab_remove(ui_tab_t *tab) {
  // Look for any tabs that have this one as parent, and let those inherit this tab's parent
  GList *n = ui_tabs;
  for(; n; n=n->next) {
    ui_tab_t *t = n->data;
    if(t->parent == tab)
      t->parent = tab->parent;
  }
  // If this tab was selected, select its parent or a neighbour
  GList *cur = g_list_find(ui_tabs, tab);
  if(cur == ui_tab_cur) {
    GList *par = tab->parent ? g_list_find(ui_tabs, tab->parent) : NULL;
    ui_tab_cur = par && par != cur ? par : cur->prev ? cur->prev : cur->next;
  }
  // And remove the tab
  ui_tabs = g_list_delete_link(ui_tabs, cur);
}


void ui_init() {
  ui_msg_tabs = g_hash_table_new(g_int64_hash, g_int64_equal);

  // global textinput field
  ui_global_textinput = ui_textinput_create(TRUE, cmd_suggest);

  // first tab = main tab
  ui_tab_open(uit_main_create(), TRUE, NULL);

  // init curses
  initscr();
  raw();
  noecho();
  curs_set(0);
  keypad(stdscr, 1);
  nodelay(stdscr, 1);

  ui_colors_init();

  // draw
  ui_draw();
}


static void ui_draw_status() {
  if(fl_refresh_queue && fl_refresh_queue->head)
    mvaddstr(winrows-1, 0, "[Refreshing share]");
  else if(fl_hash_queue && g_hash_table_size(fl_hash_queue))
    mvprintw(winrows-1, 0, "[Hashing: %d / %s / %.2f MiB/s]",
      g_hash_table_size(fl_hash_queue), str_formatsize(fl_hash_queue_size), ((float)ratecalc_rate(&fl_hash_rate))/(1024.0f*1024.0f));
  mvprintw(winrows-1, wincols-37, "[U/D:%6d/%6d KiB/s]", ratecalc_rate(&net_out)/1024, ratecalc_rate(&net_in)/1024);
  mvprintw(winrows-1, wincols-11, "[S:%3d/%3d]", cc_slots_in_use(NULL), var_get_int(0, VAR_slots));

  ui_m_updated = FALSE;
  if(ui_m_text) {
    mvaddstr(winrows-1, 0, ui_m_text);
    mvaddstr(winrows-1, str_columns(ui_m_text), "   ");
  }
}


#define tabcol(t, n) (2+ceil(log10((n)+1))+str_columns(((ui_tab_t *)(t)->data)->name))
#define prio2a(p) ((p) == UIP_LOW ? UIC(tabprio_low) : (p) == UIP_MED ? UIC(tabprio_med) : UIC(tabprio_high))

/* All tabs are in one of the following states:
 * - Selected                 (tab == ui_tab_cur->data) = sel    "n:name" in A_BOLD
 * - No change                (!sel && tab->prio == UIP_EMPTY)   "n:name" normal
 * - Change, low priority     (!sel && tab->prio == UIP_LOW)     "n!name", with ! in UIC(tabprio_low)
 * - Change, medium priority  (!sel && tab->prio == UIP_MED)     "n!name", with ! in ^_MED
 * - Change, high priority    (!sel && tab->prio == UIP_HIGH)    "n!name", with ! in ^_HIGH
 *
 * The truncated indicators are in the following states:
 * - No changes    ">>" or "<<"
 * - Change        "!>" or "<!"  with ! in same color as above
 */

static void ui_draw_tablist(int xoffset) {
  static int top = 0;
  int i, w;
  GList *n;

  int cur = g_list_position(ui_tabs, ui_tab_cur);
  int maxw = wincols-xoffset-5;

  // Make sure cur is visible
  if(top > cur)
    top = cur;
  do {
    w = maxw;
    i = top;
    for(n=g_list_nth(ui_tabs, top); n; n=n->next) {
      w -= tabcol(n, ++i);
      if(w < 0 || n == ui_tab_cur)
        break;
    }
  } while(top != cur && w < 0 && ++top);

  // display some more tabs when there is still room left
  while(top > 0 && w > tabcol(g_list_nth(ui_tabs, top-1), top-1)) {
    top--;
    w -= tabcol(g_list_nth(ui_tabs, top), top);
  }

  // check highest priority of hidden tabs before top
  // (This also sets n and i to the start of the visible list)
  char maxprio = 0;
  for(n=ui_tabs,i=0; i<top; i++,n=n->next) {
    ui_tab_t *t = n->data;
    if(t->prio > maxprio)
      maxprio = t->prio;
  }

  // print left truncate indicator
  if(top > 0) {
    mvaddch(winrows-2, xoffset, '<');
    if(!maxprio)
      addch('<');
    else {
      attron(prio2a(maxprio));
      addch('!');
      attroff(prio2a(maxprio));
    }
  } else
    mvaddch(winrows-2, xoffset+1, '[');

  // print the tab list
  w = maxw;
  for(; n; n=n->next) {
    w -= tabcol(n, ++i);
    if(w < 0)
      break;
    ui_tab_t *t = n->data;
    addch(' ');
    if(n == ui_tab_cur)
      attron(A_BOLD);
    printw("%d", i);
    if(n == ui_tab_cur || !t->prio)
      addch(':');
    else {
      attron(prio2a(t->prio));
      addch('!');
      attroff(prio2a(t->prio));
    }
    addstr(t->name);
    if(n == ui_tab_cur)
      attroff(A_BOLD);
  }

  // check priority of hidden tabs after the last visible one
  GList *last = n;
  maxprio = 0;
  for(; n&&maxprio<UIP_HIGH; n=n->next) {
    ui_tab_t *t = n->data;
    if(t->prio > maxprio)
      maxprio = t->prio;
  }

  // print right truncate indicator
  if(!last)
    addstr(" ]");
  else {
    hline(' ', w + tabcol(last, i));
    if(!maxprio)
      mvaddch(winrows-2, wincols-3, '>');
    else {
      attron(prio2a(maxprio));
      mvaddch(winrows-2, wincols-3, '!');
      attroff(prio2a(maxprio));
    }
    addch('>');
  }
}
#undef tabcol
#undef prio2a


void ui_draw() {
  ui_tab_t *curtab = ui_tab_cur->data;
  curtab->prio = UIP_EMPTY;

  getmaxyx(stdscr, winrows, wincols);
  curs_set(0); // may be overridden later on by a textinput widget
  erase();

  // first line - title
  char *title = curtab->type->title(curtab);
  attron(UIC(title));
  mvhline(0, 0, ' ', wincols);
  mvaddnstr(0, 0, title, str_offset_from_columns(title, wincols));
  attroff(UIC(title));
  g_free(title);

  // second-last line - time and tab list
  mvhline(winrows-2, 0, ACS_HLINE, wincols);
  // time
  int xoffset = 0;
  char *tfmt = var_get(0, VAR_ui_time_format);
  if(strcmp(tfmt, "-") != 0) {
#if GLIB_CHECK_VERSION(2,26,0)
    GDateTime *tm = g_date_time_new_now_local();
    char *ts = g_date_time_format(tm, tfmt);
    mvaddstr(winrows-2, 1, ts);
    xoffset = 2 + str_columns(ts);
    g_free(ts);
    g_date_time_unref(tm);
#else
    // Pre-2.6 users will have a possible buffer overflow and a slightly
    // different formatting function. Just fucking update your system already!
    time_t tm = time(NULL);
    char ts[250];
    strftime(ts, 11, tfmt, localtime(&tm));
    mvaddstr(winrows-2, 1, ts);
    xoffset = 2 + str_columns(ts);
#endif
  }
  // tabs
  ui_draw_tablist(xoffset);

  // last line - status info or notification
  ui_draw_status();

  // tab contents
  curtab->type->draw(curtab);

  refresh();
  if(ui_beep) {
    beep();
    ui_beep = FALSE;
  }
}


gboolean ui_checkupdate() {
  ui_tab_t *cur = ui_tab_cur->data;
  return ui_m_updated || ui_beep || (cur->log && cur->log->updated);
}


// Called when the day has changed. Argument is new date.
void ui_daychange(const char *day) {
  char *msg = g_strdup_printf("Day changed to %s", day);
  GList *n = ui_tabs;
  for(; n; n=n->next) {
    ui_tab_t *t = n->data;
    if(t->log)
      ui_logwindow_addline(t->log, msg, TRUE, TRUE);
  }
  g_free(msg);
}


void ui_input(guint64 key) {
  ui_tab_t *curtab = ui_tab_cur->data;

  switch(key) {
  case INPT_CTRL('c'): // ctrl+c
    ui_m(NULL, UIM_NOLOG, "Type /quit to exit ncdc.");
    break;
  case INPT_ALT('j'): // alt+j (previous tab)
    ui_tab_cur = ui_tab_cur->prev ? ui_tab_cur->prev : g_list_last(ui_tabs);
    break;
  case INPT_ALT('k'): // alt+k (next tab)
    ui_tab_cur = ui_tab_cur->next ? ui_tab_cur->next : ui_tabs;
    break;
  case INPT_ALT('h'): ; // alt+h (swap tab with left)
    GList *prev = ui_tab_cur->prev;
    if(prev) {
      ui_tabs = g_list_delete_link(ui_tabs, ui_tab_cur);
      ui_tabs = g_list_insert_before(ui_tabs, prev, curtab);
      ui_tab_cur = prev->prev;
    }
    break;
  case INPT_ALT('l'): ; // alt+l (swap tab with right)
    GList *next = ui_tab_cur->next;
    if(next) {
      ui_tabs = g_list_delete_link(ui_tabs, ui_tab_cur);
      ui_tabs = g_list_insert_before(ui_tabs, next->next, curtab);
      ui_tab_cur = next->next;
    }
    break;
  case INPT_ALT('c'): // alt+c (alias for /close)
    cmd_handle("/close");
    break;
  case INPT_CTRL('l'): // ctrl+l (alias for /clear)
    cmd_handle("/clear");
    break;

  case INPT_ALT('r'): // alt+r (alias for /refresh)
    cmd_handle("/refresh");
    break;

  case INPT_ALT('o'): // alt+o (alias for /browse)
    cmd_handle("/browse");
    break;

  case INPT_ALT('n'): // alt+n (alias for /connections)
    cmd_handle("/connections");
    break;

  case INPT_ALT('q'): // alt+q (alias for /queue)
    cmd_handle("/queue");
    break;

  default:
    // alt+num (switch tab)
    if(key >= INPT_ALT('0') && key <= INPT_ALT('9')) {
      GList *n = g_list_nth(ui_tabs, INPT_CODE(key) == '0' ? 9 : INPT_CODE(key)-'1');
      if(n)
        ui_tab_cur = n;
    // let tab handle it
    } else
      curtab->type->key(curtab, key);
  }
}

