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
#include "uit_conn.h"


ui_tab_type_t uit_conn[1];


typedef struct tab_t {
  ui_tab_t tab;
  ui_listing_t *list;
  gboolean details;
} tab_t;


// There can only be at most one connections tab.
static tab_t *conntab;



static gint sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const cc_t *a = da;
  const cc_t *b = db;
  int o = 0;
  if(!o && !a->nick != !b->nick)
    o = a->nick ? 1 : -1;
  if(!o && a->nick && b->nick)
    o = strcmp(a->nick, b->nick);
  if(!o && a->hub && b->hub)
    o = strcmp(a->hub->tab->name, b->hub->tab->name);
  return o;
}


ui_tab_t *uit_conn_create() {
  g_return_val_if_fail(!conntab, NULL);

  tab_t *tab = conntab = g_new0(tab_t, 1);
  tab->tab.name = "connections";
  tab->tab.type = uit_conn;
  // sort the connection list
  g_sequence_sort(cc_list, sort_func, NULL);
  tab->list = ui_listing_create(cc_list);
  return (ui_tab_t *)tab;
}


static void t_close(ui_tab_t *tab) {
  g_return_if_fail((ui_tab_t *)conntab == tab);

  ui_tab_remove(tab);
  ui_listing_free(conntab->list);
  g_free(conntab);
  conntab = NULL;
}


static char *t_title() {
  return g_strdup("Connection list");
}


static void t_draw_row(ui_listing_t *list, GSequenceIter *iter, int row, void *dat) {
  cc_t *cc = g_sequence_get(iter);

  attron(iter == list->sel ? UIC(list_select) : UIC(list_default));
  mvhline(row, 0, ' ', wincols);
  if(iter == list->sel)
    mvaddstr(row, 0, ">");

  mvaddch(row, 2,
    cc->state == CCS_CONN      ? 'C' :
    cc->state == CCS_DISCONN   ? '-' :
    cc->state == CCS_HANDSHAKE ? 'H' :
    cc->state == CCS_IDLE      ? 'I' : cc->dl ? 'D' : 'U');
  mvaddch(row, 3, cc->tls ? 't' : ' ');

  if(cc->nick)
    mvaddnstr(row, 5, cc->nick, str_offset_from_columns(cc->nick, 15));
  else {
    char tmp[30];
    strcpy(tmp, "IP:");
    strcat(tmp, cc->remoteaddr);
    if(strchr(tmp+3, ':'))
      *(strchr(tmp+3, ':')) = 0;
    mvaddstr(row, 5, tmp);
  }

  if(cc->hub)
    mvaddnstr(row, 21, cc->hub->tab->name, str_offset_from_columns(cc->hub->tab->name, 11));


  mvaddstr(row, 33, cc->last_length ? str_formatsize(cc->last_length) : "-");

  if(cc->last_length && !cc->timeout_src) {
    float length = cc->last_length;
    mvprintw(row, 45, "%3.0f%%", (length-net_left(cc->net))*100.0f/length);
  } else
    mvaddstr(row, 45, " -");

  if(cc->timeout_src)
    mvaddstr(row, 50, "     -");
  else
    mvprintw(row, 50, "%6d", ratecalc_rate(cc->dl ? net_rate_in(cc->net) : net_rate_out(cc->net))/1024);

  if(cc->err) {
    mvaddstr(row, 58, "Disconnected: ");
    addnstr(cc->err->message, str_offset_from_columns(cc->err->message, wincols-(58+14)));
  } else if(cc->last_file) {
    char *file = strrchr(cc->last_file, '/');
    if(file)
      file++;
    else
      file = cc->last_file;
      mvaddnstr(row, 58, file, str_offset_from_columns(file, wincols-58));
  }

  attroff(iter == list->sel ? UIC(list_select) : UIC(list_default));
}


static void t_draw_details(tab_t *t, int l) {
  cc_t *cc = g_sequence_iter_is_end(t->list->sel) ? NULL : g_sequence_get(t->list->sel);
  if(!cc) {
    mvaddstr(l+1, 0, "Nothing selected.");
    return;
  }

  // labels
  attron(A_BOLD);
  mvaddstr(l+1,  3, "Username:");
  mvaddstr(l+1, 42, "Hub:");
  mvaddstr(l+2,  4, "IP/port:");
  mvaddstr(l+2, 39, "Status:");
  mvaddstr(l+3,  9, "Up:");
  mvaddstr(l+3, 41, "Down:");
  mvaddstr(l+4,  7, "Size:");
  mvaddstr(l+5,  5, "Offset:");
  mvaddstr(l+6,  6, "Chunk:");
  mvaddstr(l+4, 37, "Progress:");
  mvaddstr(l+5, 42, "ETA:");
  mvaddstr(l+6, 41, "Idle:");
  mvaddstr(l+7,  7, "File:");
  mvaddstr(l+8,  1, "Last error:");
  attroff(A_BOLD);

  // line 1
  mvaddstr(l+1, 13, cc->nick ? cc->nick : "Unknown / connecting");
  mvaddstr(l+1, 47, cc->hub ? cc->hub->tab->name : "-");
  // line 2
  mvaddstr(l+2, 13, cc->remoteaddr);
  mvaddstr(l+2, 47,
    cc->state == CCS_CONN      ? "Connecting" :
    cc->state == CCS_DISCONN   ? "Disconnected" :
    cc->state == CCS_HANDSHAKE ? "Handshake" :
    cc->state == CCS_IDLE      ? "Idle" : cc->dl ? "Downloading" : "Uploading");
  // line 3
  mvprintw(l+3, 13, "%d KiB/s (%s)", ratecalc_rate(net_rate_out(cc->net))/1024, str_formatsize(ratecalc_total(net_rate_out(cc->net))));
  mvprintw(l+3, 47, "%d KiB/s (%s)", ratecalc_rate(net_rate_in(cc->net))/1024, str_formatsize(ratecalc_total(net_rate_in(cc->net))));
  // size / offset / chunk (line 4/5/6)
  mvaddstr(l+4, 13, cc->last_size ? str_formatsize(cc->last_size) : "-");
  mvaddstr(l+5, 13, cc->last_size ? str_formatsize(cc->last_offset) : "-");
  mvaddstr(l+6, 13, cc->last_length ? str_formatsize(cc->last_length) : "-");
  // progress / eta / idle (line 4/5/6)
  if(cc->last_length && !cc->timeout_src) {
    float length = cc->last_length;
    mvprintw(l+4, 47, "%3.0f%%", (length-net_left(cc->net))*100.0f/length);
  } else
    mvaddstr(l+4, 47, "-");
  if(cc->last_length && !cc->timeout_src)
    mvaddstr(l+5, 47, ratecalc_eta(cc->dl ? net_rate_in(cc->net) : net_rate_out(cc->net), net_left(cc->net)));
  else
    mvaddstr(l+5, 47, "-");
  mvprintw(l+6, 47, "%ds", (int)(time(NULL)-net_last_activity(cc->net)));
  // line 7
  if(cc->last_file)
    mvaddnstr(l+7, 13, cc->last_file, str_offset_from_columns(cc->last_file, wincols-13));
  else
    mvaddstr(l+7, 13, "None.");
  // line 8
  if(cc->err)
    mvaddnstr(l+8, 13, cc->err->message, str_offset_from_columns(cc->err->message, wincols-13));
  else
    mvaddstr(l+8, 13, "-");
}


static void t_draw(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;

  attron(UIC(list_header));
  mvhline(1, 0, ' ', wincols);
  mvaddstr(1, 2,  "St Username");
  mvaddstr(1, 21, "Hub");
  mvaddstr(1, 33, "Chunk          %");
  mvaddstr(1, 50, " KiB/s");
  mvaddstr(1, 58, "File");
  attroff(UIC(list_header));

  int bottom = t->details ? winrows-11 : winrows-3;
  int pos = ui_listing_draw(t->list, 2, bottom-1, t_draw_row, NULL);

  // footer
  attron(UIC(separator));
  mvhline(bottom, 0, ' ', wincols);
  mvprintw(bottom, wincols-24, "%3d connections    %3d%%", g_sequence_iter_get_position(g_sequence_get_end_iter(t->list->list)), pos);
  attroff(UIC(separator));

  // detailed info
  if(t->details)
    t_draw_details(t, bottom);
}


static void t_key(ui_tab_t *tab, guint64 key) {
  tab_t *t = (tab_t *)tab;
  if(ui_listing_key(t->list, key, (winrows-10)/2))
    return;

  cc_t *cc = g_sequence_iter_is_end(t->list->sel) ? NULL : g_sequence_get(t->list->sel);

  switch(key) {
  case INPT_CHAR('?'):
    uit_main_keys("connections");
    break;
  case INPT_CTRL('j'): // newline
  case INPT_CHAR('i'): // i - toggle detailed info
    t->details = !t->details;
    break;
  case INPT_CHAR('f'): // f - find user
    if(!cc)
      ui_m(NULL, 0, "Nothing selected.");
    else if(!cc->hub || !cc->uid)
      ui_m(NULL, 0, "User or hub unknown.");
    else if(!ui_hub_finduser(cc->hub->tab, cc->uid, NULL, FALSE))
      ui_m(NULL, 0, "User has left the hub.");
    break;
  case INPT_CHAR('m'): // m - /msg user
    if(!cc)
      ui_m(NULL, 0, "Nothing selected.");
    else if(!cc->hub || !cc->uid)
      ui_m(NULL, 0, "User or hub unknown.");
    else {
      ui_tab_t *t = g_hash_table_lookup(ui_msg_tabs, &cc->uid);
      if(t) {
        ui_tab_cur = g_list_find(ui_tabs, t);
      } else {
        hub_user_t *u = g_hash_table_lookup(hub_uids, &cc->uid);
        if(!u) {
          ui_m(NULL, 0, "User has left the hub.");
        } else {
          t = ui_msg_create(cc->hub, u);
          ui_tab_open(t, TRUE, t);
        }
      }
    }
    break;
  case INPT_CHAR('d'): // d - disconnect
    if(!cc)
      ui_m(NULL, 0, "Nothing selected.");
    else if(net_is_idle(cc->net))
      ui_m(NULL, 0, "Not connected.");
    else
      cc_disconnect(cc, FALSE);
    break;
  case INPT_CHAR('q'): // q - find queue item
    if(!cc)
      ui_m(NULL, 0, "Nothing selected.");
    else if(!cc->dl || !cc->last_file)
      ui_m(NULL, 0, "Not downloading a file.");
    else {
      dl_t *dl = g_hash_table_lookup(dl_queue, cc->last_hash);
      if(!dl)
        ui_m(NULL, 0, "File has been removed from the queue.");
      else {
        if(ui_dl_tab)
          ui_tab_cur = g_list_find(ui_tabs, ui_dl_tab);
        else
          ui_tab_open(ui_dl_create(), TRUE, tab);
        ui_dl_select(dl, cc->uid);
      }
    }
    break;
  }
}


#if INTERFACE
#define UITCONN_ADD 0
#define UITCONN_DEL 1
#define UITCONN_MOD 2  // when the nick or hub changes
#endif


void uit_conn_listchange(GSequenceIter *iter, int change) {
  if(!conntab)
    return;

  switch(change) {
  case UITCONN_ADD:
    g_sequence_sort_changed(iter, sort_func, NULL);
    ui_listing_inserted(conntab->list);
    break;
  case UITCONN_DEL:
    ui_listing_remove(conntab->list, iter);
    break;
  case UITCONN_MOD:
    g_sequence_sort_changed(iter, sort_func, NULL);
    ui_listing_sorted(conntab->list);
    break;
  }
}


// Opens the conn tab (if it's not open yet) and selects the specified cc item,
// if that's not NULL.
void uit_conn_open(cc_t *cc, ui_tab_t *parent) {
  if(conntab)
    ui_tab_cur = g_list_find(ui_tabs, conntab);
  else
    ui_tab_open(uit_conn_create(), TRUE, parent);

  // cc->iter should be valid at this point
  if(cc)
    conntab->list->sel = cc->iter;
}


ui_tab_type_t uit_conn[1] = { { t_draw, t_title, t_key, t_close } };

