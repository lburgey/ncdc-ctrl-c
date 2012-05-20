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
#include "uit_msg.h"


ui_tab_type_t uit_msg[1];


typedef struct tab_t {
  ui_tab_t tab;
  guint64 uid;
  int replyto;
} tab_t;


// uid -> tab_t lookup table.
static GHashTable *msg_tabs = NULL;

#define inittable() if(!msg_tabs) msg_tabs = g_hash_table_new(g_int64_hash, g_int64_equal);


static int log_checkchat(void *dat, char *nick, char *msg) {
  ui_tab_t *tab = ((ui_tab_t *)dat)->hub->tab;
  return tab->hub_nick && strcmp(nick, tab->hub_nick) == 0 ? 2 : 0;
}


ui_tab_t *uit_msg_create(hub_t *hub, hub_user_t *user) {
  inittable();
  g_return_val_if_fail(!g_hash_table_lookup(msg_tabs, &user->uid), NULL);

  tab_t *t = g_new0(tab_t, 1);
  t->tab.type = uit_msg;
  t->tab.hub = hub;
  t->uid = user->uid;
  t->tab.name = g_strdup_printf("~%s", user->name);
  t->tab.log = ui_logwindow_create(t->tab.name, var_get_int(0, VAR_backlog));
  t->tab.log->handle = t;
  t->tab.log->checkchat = log_checkchat;

  ui_mf((ui_tab_t *)t, 0, "Chatting with %s on %s.", user->name, hub->tab->name);
  g_hash_table_insert(msg_tabs, &t->uid, t);

  return (ui_tab_t *)t;
}


static void t_close(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;
  g_hash_table_remove(msg_tabs, &t->uid);
  ui_tab_remove(tab);
  ui_logwindow_free(tab->log);
  g_free(tab->name);
  g_free(tab);
}


static void t_draw(ui_tab_t *tab) {
  ui_logwindow_draw(tab->log, 1, 0, winrows-4, wincols);

  mvaddstr(winrows-3, 0, tab->name);
  addstr("> ");
  int pos = str_columns(tab->name)+2;
  ui_textinput_draw(ui_global_textinput, winrows-3, pos, wincols-pos);
}


static char *t_title(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;
  return g_strdup_printf("Chatting with %s on %s%s.",
    tab->name+1, tab->hub->tab->name, g_hash_table_lookup(hub_uids, &t->uid) ? "" : " (offline)");
}


static void t_key(ui_tab_t *tab, guint64 key) {
  char *str = NULL;
  if(!ui_logwindow_key(tab->log, key, winrows) &&
      ui_textinput_key(ui_global_textinput, key, &str) && str) {
    cmd_handle(str);
    g_free(str);
  }
}


// Called from the hub tab when user info changes. A QUIT is always done before
// the user is removed from the hub_uids table.
void uit_msg_userchange(hub_user_t *u, int change) {
  inittable();
  tab_t *t = g_hash_table_lookup(msg_tabs, &u->uid);
  if(!t)
    return;

  switch(change) {
  case UIHUB_UC_JOIN:
    ui_mf((ui_tab_t *)t, 0, "--> %s has joined.", u->name);
    break;
  case UIHUB_UC_QUIT:
    ui_mf((ui_tab_t *)t, 0, "--< %s has quit.", u->name);
    break;
  case UIHUB_UC_NFO:
    // Detect nick changes.
    // Note: the name of the log file remains the same even after a nick
    // change. This probably isn't a major problem, though. Nick changes are
    // not very common and are only detected on ADC hubs.
    if(strcmp(u->name, t->tab.name+1) != 0) {
      ui_mf((ui_tab_t *)t, 0, "%s is now known as %s.", t->tab.name+1, u->name);
      g_free(t->tab.name);
      t->tab.name = g_strdup_printf("~%s", u->name);
    }
    break;
  }
}


// Opens a msg tab for specified uid. Returns FALSE if no such uid exists.
gboolean uit_msg_open(guint64 uid, ui_tab_t *parent) {
  inittable();
  ui_tab_t *t = g_hash_table_lookup(msg_tabs, &uid);
  if(t) {
    ui_tab_cur = g_list_find(ui_tabs, t);
    return TRUE;
  }

  hub_user_t *u = g_hash_table_lookup(hub_uids, &uid);
  if(!u)
    return FALSE;
  t = uit_msg_create(u->hub, u);
  ui_tab_open(t, TRUE, parent);
  return TRUE;
}


// Called from the hub tab when a message has arrived.
void uit_msg_msg(hub_user_t *user, const char *msg, int replyto) {
  inittable();
  ui_tab_t *t = g_hash_table_lookup(msg_tabs, &user->uid);
  if(!t) {
    t = uit_msg_create(user->hub, user);
    ui_tab_open(t, FALSE, user->hub->tab);
  }

  ui_m(t, UIP_HIGH, msg);
  ((tab_t *)t)->replyto = replyto;
}


// Called when a hub has been disconnected. I.e. all users on that hub are now
// offline.
void uit_msg_disconnect(hub_t *hub) {
  inittable();

  GHashTableIter i;
  g_hash_table_iter_init(&i, msg_tabs);
  tab_t *t = NULL;
  while(g_hash_table_iter_next(&i, NULL, (gpointer *)&t)) {
    // The user could have quit while we kept this tab open, so check that the
    // user was still online when the disconnect happened.
    if(g_hash_table_lookup(hub_uids, &t->uid))
      ui_mf((ui_tab_t *)t, 0, "--< %s has quit.", t->tab.name+1);
  }
}


guint64 uit_msg_uid(ui_tab_t *tab) {
  return ((tab_t *)tab)->uid;
}


int uit_msg_replyto(ui_tab_t *tab) {
  return ((tab_t *)tab)->replyto;
}


ui_tab_type_t uit_msg[1] = { { t_draw, t_title, t_key, t_close } };

