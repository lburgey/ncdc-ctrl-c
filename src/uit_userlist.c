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
#include "uit_userlist.h"
#include <math.h>


ui_tab_type_t uit_userlist[1];


typedef struct tab_t {
  ui_tab_t tab;
  ui_listing_t *list;
  int order;
  gboolean reverse : 1;
  gboolean details : 1;
  gboolean opfirst : 1;
  gboolean hide_desc : 1;
  gboolean hide_tag : 1;
  gboolean hide_mail : 1;
  gboolean hide_conn : 1;
  gboolean hide_ip : 1;
} tab_t;



// Columns to sort on
#define SORT_USER   0
#define SORT_SHARE  1
#define SORT_CONN   2
#define SORT_DESC   3
#define SORT_MAIL   4
#define SORT_CLIENT 5
#define SORT_IP     6


static gint sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const hub_user_t *a = da;
  const hub_user_t *b = db;
  tab_t *t = dat;
  int p = t->order;

  if(t->opfirst && !a->isop != !b->isop)
    return a->isop && !b->isop ? -1 : 1;

  // All orders have the username as secondary order.
  int o = p == SORT_USER ? 0 :
    p == SORT_SHARE  ? a->sharesize > b->sharesize ? 1 : -1:
    p == SORT_CONN   ? (t->tab.hub->adc ? a->conn - b->conn : strcmp(a->conn?a->conn:"", b->conn?b->conn:"")) :
    p == SORT_DESC   ? g_utf8_collate(a->desc?a->desc:"", b->desc?b->desc:"") :
    p == SORT_MAIL   ? g_utf8_collate(a->mail?a->mail:"", b->mail?b->mail:"") :
    p == SORT_CLIENT ? strcmp(a->client?a->client:"", b->client?b->client:"")
                     : ip4_cmp(a->ip4, b->ip4);

  // Username sort
  if(!o)
    o = g_utf8_collate(a->name, b->name);
  if(!o && a->name_hub && b->name_hub)
    o = strcmp(a->name_hub, b->name_hub);
  if(!o)
    o = a - b;
  return t->reverse ? -1*o : o;
}


ui_tab_t *uit_userlist_create(hub_t *hub) {
  tab_t *t = g_new0(tab_t, 1);
  t->tab.name = g_strdup_printf("@%s", hub->tab->name+1);
  t->tab.type = uit_userlist;
  t->tab.hub = hub;
  t->opfirst = TRUE;
  t->hide_conn = TRUE;
  t->hide_mail = TRUE;
  t->hide_ip = TRUE;

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
    u->iter = g_sequence_insert_sorted(users, u, sort_func, t);
  t->list = ui_listing_create(users);

  return (ui_tab_t *)t;
}


static void t_close(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;
  t->tab.hub->tab->userlist_tab = NULL;
  ui_tab_remove(tab);
  // To clean things up, we should also reset all hub_user->iter fields. But
  // this isn't all that necessary since they won't be used anymore until they
  // get reset in a subsequent ui_userlist_create().
  g_sequence_free(t->list->list);
  ui_listing_free(t->list);
  g_free(t->tab.name);
  g_free(t);
}


static char *t_title(ui_tab_t *tab) {
  return g_strdup_printf("%s / User list", tab->hub->tab->name);
}


#define DRAW_COL(row, colvar, width, str) do {\
    if(width > 1)\
      mvaddnstr(row, colvar, str, str_offset_from_columns(str, width-1));\
    colvar += width;\
  } while(0)


typedef struct draw_opts_t {
  int cw_user, cw_share, cw_conn, cw_desc, cw_mail, cw_tag, cw_ip;
} draw_opts_t;


static void draw_row(ui_listing_t *list, GSequenceIter *iter, int row, void *dat) {
  hub_user_t *user = g_sequence_get(iter);
  draw_opts_t *o = dat;

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
static void calc_widths(tab_t *t, draw_opts_t *o) {
  // available width
  int w = wincols-5;

  // share has a fixed size
  o->cw_share = 12;
  w -= 12;

  // IP column as well
  o->cw_ip = t->hide_ip ? 0 : 16;
  w -= o->cw_ip;

  // User column has a minimum size (but may grow a bit later on, so will still be counted as a column)
  o->cw_user = 15;
  w -= 15;

  // Total weight (first one is for the user column)
  double wt = 0.02
    + (t->hide_conn ? 0.0 : 0.16)
    + (t->hide_desc ? 0.0 : 0.32)
    + (t->hide_mail ? 0.0 : 0.18)
    + (t->hide_tag  ? 0.0 : 0.32);

  // Scale factor
  double ws = 1.0 + (
    + (t->hide_conn ? 0.16: 0.0)
    + (t->hide_desc ? 0.32: 0.0)
    + (t->hide_mail ? 0.18: 0.0)
    + (t->hide_tag  ? 0.32: 0.0))/wt;
  // scale to available width
  ws *= w;

  // Get the column widths. Note the use of floor() here, this prevents that
  // the total width exceeds the available width. The remaining columns will be
  // given to the user column, which is always present anyway.
  o->cw_conn = t->hide_conn ? 0 : floor(0.16*ws);
  o->cw_desc = t->hide_desc ? 0 : floor(0.32*ws);
  o->cw_mail = t->hide_mail ? 0 : floor(0.18*ws);
  o->cw_tag  = t->hide_tag  ? 0 : floor(0.32*ws);
  o->cw_user += w - o->cw_conn - o->cw_desc - o->cw_mail - o->cw_tag;
}


static void t_draw(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;

  draw_opts_t o;
  calc_widths(t, &o);

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
  int bottom = t->details ? winrows-7 : winrows-3;
  int pos = ui_listing_draw(t->list, 2, bottom-1, draw_row, &o);

  // footer
  attron(UIC(separator));
  mvhline(bottom, 0, ' ', wincols);
  int count = g_hash_table_size(t->tab.hub->users);
  mvaddstr(bottom, 0, "Totals:");
  mvprintw(bottom, o.cw_user+5, "%s%c   %d users",
    str_formatsize(t->tab.hub->sharesize), t->tab.hub->sharecount == count ? ' ' : '+', count);
  mvprintw(bottom, wincols-6, "%3d%%", pos);
  attroff(UIC(separator));

  // detailed info box
  if(!t->details)
    return;
  if(g_sequence_iter_is_end(t->list->sel))
    mvaddstr(bottom+1, 2, "No user selected.");
  else {
    hub_user_t *u = g_sequence_get(t->list->sel);
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


static void t_key(ui_tab_t *tab, guint64 key) {
  tab_t *t = (tab_t *)tab;

  if(ui_listing_key(t->list, key, winrows/2))
    return;

  hub_user_t *sel = g_sequence_iter_is_end(t->list->sel) ? NULL : g_sequence_get(t->list->sel);
  gboolean sort = FALSE;
  switch(key) {
  case INPT_CHAR('?'):
    uit_main_keys("userlist");
    break;

  // Sorting
#define SETSORT(c) \
  t->reverse = t->order == c ? !t->reverse : FALSE;\
  t->order = c;\
  sort = TRUE;

  case INPT_CHAR('s'): // s/S - sort on share size
  case INPT_CHAR('S'):
    SETSORT(SORT_SHARE);
    break;
  case INPT_CHAR('u'): // u/U - sort on username
  case INPT_CHAR('U'):
    SETSORT(SORT_USER)
    break;
  case INPT_CHAR('D'): // D - sort on description
    SETSORT(SORT_DESC)
    break;
  case INPT_CHAR('T'): // T - sort on client (= tag)
    SETSORT(SORT_CLIENT)
    break;
  case INPT_CHAR('E'): // E - sort on email
    SETSORT(SORT_MAIL)
    break;
  case INPT_CHAR('C'): // C - sort on connection
    SETSORT(SORT_CONN)
    break;
  case INPT_CHAR('P'): // P - sort on IP
    SETSORT(SORT_IP)
    break;
  case INPT_CHAR('o'): // o - toggle sorting OPs before others
    t->opfirst = !t->opfirst;
    sort = TRUE;
    break;
#undef SETSORT

  // Column visibility
  case INPT_CHAR('d'): // d (toggle description visibility)
    t->hide_desc = !t->hide_desc;
    break;
  case INPT_CHAR('t'): // t (toggle tag visibility)
    t->hide_tag = !t->hide_tag;
    break;
  case INPT_CHAR('e'): // e (toggle e-mail visibility)
    t->hide_mail = !t->hide_mail;
    break;
  case INPT_CHAR('c'): // c (toggle connection visibility)
    t->hide_conn = !t->hide_conn;
    break;
  case INPT_CHAR('p'): // p (toggle IP visibility)
    t->hide_ip = !t->hide_ip;
    break;

  case INPT_CTRL('j'): // newline
  case INPT_CHAR('i'): // i       (toggle user info)
    t->details = !t->details;
    break;
  case INPT_CHAR('m'): // m (/msg user)
    if(!sel)
      ui_m(NULL, 0, "No user selected.");
    else
      uit_msg_open(sel->uid, tab);
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

  if(sort) {
    g_sequence_sort(t->list->list, sort_func, tab);
    ui_listing_sorted(t->list);
    ui_mf(NULL, 0, "Ordering by %s (%s%s)",
        t->order == SORT_USER  ? "user name" :
        t->order == SORT_SHARE ? "share size" :
        t->order == SORT_CONN  ? "connection" :
        t->order == SORT_DESC  ? "description" :
        t->order == SORT_MAIL  ? "e-mail" :
        t->order == SORT_CLIENT? "tag" : "IP address",
      t->reverse ? "descending" : "ascending", t->opfirst ? ", OPs first" : "");
  }
}


// Called when the hub is disconnected. All users should be removed in one go,
// this is faster than a _userchange() for every user.
void uit_userlist_disconnect(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;

  g_sequence_free(t->list->list);
  ui_listing_free(t->list);
  t->list = ui_listing_create(g_sequence_new(NULL));
}


// Called from the hub tab when something changes to the user list.
void uit_userlist_userchange(ui_tab_t *tab, int change, hub_user_t *user) {
  tab_t *t = (tab_t *)tab;

  if(change == UIHUB_UC_JOIN) {
    user->iter = g_sequence_insert_sorted(t->list->list, user, sort_func, t);
    ui_listing_inserted(t->list);
  } else if(change == UIHUB_UC_QUIT) {
    g_return_if_fail(g_sequence_get(user->iter) == (gpointer)user);
    ui_listing_remove(t->list, user->iter);
    g_sequence_remove(user->iter);
  } else {
    g_sequence_sort_changed(user->iter, sort_func, t);
    ui_listing_sorted(t->list);
  }
}


// Opens the user list for a hub and selects the user specified by uid or
// user/utf8. Returns FALSE if a user was specified but could not be found.
gboolean uit_userlist_open(hub_t *hub, guint64 uid, const char *user, gboolean utf8) {
  hub_user_t *u =
    !uid && !user ? NULL :
    uid ? g_hash_table_lookup(hub_uids, &uid) :
    utf8 ? hub_user_get(hub, user) : g_hash_table_lookup(hub->users, user);
  if((uid || user) && (!u || u->hub != hub))
    return FALSE;

  if(hub->tab->userlist_tab)
    ui_tab_cur = g_list_find(ui_tabs, hub->tab->userlist_tab);
  else {
    hub->tab->userlist_tab = uit_userlist_create(hub);
    ui_tab_open(hub->tab->userlist_tab, TRUE, hub->tab);
  }

  if(u) {
    tab_t *t = (tab_t *)hub->tab->userlist_tab;
    // u->iter should be valid at this point.
    t->list->sel = u->iter;
    t->details = TRUE;
  }
  return TRUE;
}


ui_tab_type_t uit_userlist[1] = { { t_draw, t_title, t_key, t_close } };

