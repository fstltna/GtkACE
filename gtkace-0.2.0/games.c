/*
 * games.c - the list of known games, the game chooser and the login
 * dialog (WinACE frmGameChoice and frmEmpireLogin).
 */
#include <string.h>
#include <glib/gstdio.h>
#include "gtkace.h"

#define GENERAL "GtkACE"

static char *
config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "gtkace",
                            "games.conf", NULL);
}

Game *
game_new(void)
{
    Game *g = g_new0(Game, 1);

    g->name = g_strdup("");
    g->host = g_strdup("localhost");
    g->port = 6665;
    g->country = g_strdup("");
    g->password = g_strdup("");
    g->user = g_strdup(g_get_user_name());
    g->autoupdate = TRUE;
    g->utf8 = TRUE;
    return g;
}

Game *
game_copy(const Game *s)
{
    Game *g = g_new0(Game, 1);

    *g = *s;
    g->name = g_strdup(s->name);
    g->host = g_strdup(s->host);
    g->country = g_strdup(s->country);
    g->password = g_strdup(s->password);
    g->user = g_strdup(s->user);
    return g;
}

void
game_free(Game *g)
{
    if (!g)
        return;
    g_free(g->name);
    g_free(g->host);
    g_free(g->country);
    g_free(g->password);
    g_free(g->user);
    g_free(g);
}

void
games_free(GList *games)
{
    g_list_free_full(games, (GDestroyNotify)game_free);
}

static char *
kf_str(GKeyFile *kf, const char *grp, const char *key, const char *dflt)
{
    char *s = g_key_file_get_string(kf, grp, key, NULL);

    return s ? s : g_strdup(dflt);
}

static gboolean
kf_bool(GKeyFile *kf, const char *grp, const char *key, gboolean dflt)
{
    GError *err = NULL;
    gboolean b = g_key_file_get_boolean(kf, grp, key, &err);

    if (err) {
        g_error_free(err);
        return dflt;
    }
    return b;
}

GList *
games_load(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = config_path();
    char **groups, **order;
    GList *list = NULL;
    gsize i, n;

    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);

    /* MRU order first, then any other groups */
    order = g_key_file_get_string_list(kf, GENERAL, "order", NULL, NULL);
    groups = g_key_file_get_groups(kf, &n);
    for (int pass = 0; pass < 2; pass++) {
        char **src = pass ? groups : order;

        for (i = 0; src && src[i]; i++) {
            const char *grp = src[i];
            Game *g;

            if (!strcmp(grp, GENERAL) || !g_key_file_has_group(kf, grp))
                continue;
            if (pass && order && g_strv_contains((const char *const *)order,
                                                 grp))
                continue;
            g = g_new0(Game, 1);
            g->name = g_strdup(grp);
            g->host = kf_str(kf, grp, "host", "localhost");
            g->port = g_key_file_get_integer(kf, grp, "port", NULL);
            if (g->port <= 0 || g->port > 65535)
                g->port = 6665;
            g->country = kf_str(kf, grp, "country", "");
            g->password = kf_str(kf, grp, "password", "");
            g->user = kf_str(kf, grp, "user", g_get_user_name());
            g->kill = kf_bool(kf, grp, "kill", FALSE);
            g->deity = kf_bool(kf, grp, "deity", FALSE);
            g->autoupdate = kf_bool(kf, grp, "autoupdate", TRUE);
            g->autoread = kf_bool(kf, grp, "autoread", FALSE);
            g->utf8 = kf_bool(kf, grp, "utf8", TRUE);
            list = g_list_append(list, g);
        }
    }
    g_strfreev(order);
    g_strfreev(groups);
    g_key_file_free(kf);
    return list;
}

void
games_save(GList *games)
{
    GKeyFile *kf = g_key_file_new();
    GPtrArray *order = g_ptr_array_new();
    char *path = config_path(), *dir, *data;
    GError *err = NULL;
    gsize len;
    GList *l;

    for (l = games; l; l = l->next) {
        Game *g = l->data;

        g_ptr_array_add(order, g->name);
        g_key_file_set_string(kf, g->name, "host", g->host);
        g_key_file_set_integer(kf, g->name, "port", g->port);
        g_key_file_set_string(kf, g->name, "country", g->country);
        g_key_file_set_string(kf, g->name, "password", g->password);
        g_key_file_set_string(kf, g->name, "user", g->user);
        g_key_file_set_boolean(kf, g->name, "kill", g->kill);
        g_key_file_set_boolean(kf, g->name, "deity", g->deity);
        g_key_file_set_boolean(kf, g->name, "autoupdate", g->autoupdate);
        g_key_file_set_boolean(kf, g->name, "autoread", g->autoread);
        g_key_file_set_boolean(kf, g->name, "utf8", g->utf8);
    }
    g_key_file_set_string_list(kf, GENERAL, "order",
                               (const char *const *)order->pdata, order->len);
    g_ptr_array_free(order, TRUE);

    dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    data = g_key_file_to_data(kf, &len, NULL);
    /* the file holds passwords: keep it private */
    if (!g_file_set_contents_full(path, data, len,
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, &err)) {
        g_warning("Cannot save %s: %s", path, err->message);
        g_error_free(err);
    }
    g_free(data);
    g_free(path);
    g_key_file_free(kf);
}

static GList *
games_find(GList *games, const char *name)
{
    for (GList *l = games; l; l = l->next)
        if (!strcmp(((Game *)l->data)->name, name))
            return l;
    return NULL;
}

void
games_touch(const char *name)
{
    GList *games = games_load(), *l = games_find(games, name);

    if (l && l != games) {
        games = g_list_remove_link(games, l);
        games = g_list_concat(l, games);
        games_save(games);
    }
    games_free(games);
}

char *
game_db_path(const Game *g)
{
    GString *safe = g_string_new(NULL);
    char *dir, *file, *path;

    for (const char *p = g->name; *p; p++)
        g_string_append_c(safe, g_ascii_isalnum(*p) || *p == '-' || *p == '_'
                          ? *p : '_');
    if (!safe->len)
        g_string_append(safe, "game");
    dir = g_build_filename(g_get_user_data_dir(), "gtkace", NULL);
    g_mkdir_with_parents(dir, 0700);
    file = g_strconcat(safe->str, ".db", NULL);
    path = g_build_filename(dir, file, NULL);
    g_free(file);
    g_free(dir);
    g_string_free(safe, TRUE);
    return path;
}

/* ------------------------------------------------------ login dialog */

enum { RESP_CONNECT = 1, RESP_OFFLINE = 2 };

typedef struct {
    GtkWidget *name, *host, *port, *country, *password, *user;
    GtkWidget *kill, *deity, *autoupdate, *autoread, *utf8;
} LoginWidgets;

static GtkWidget *
grid_entry(GtkGrid *grid, int row, const char *label, const char *value)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    GtkWidget *e = gtk_entry_new();

    gtk_label_set_xalign(GTK_LABEL(l), 1.0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), e);
    gtk_entry_set_text(GTK_ENTRY(e), value ? value : "");
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_widget_set_hexpand(e, TRUE);
    gtk_grid_attach(grid, l, 0, row, 1, 1);
    gtk_grid_attach(grid, e, 1, row, 1, 1);
    return e;
}

static GtkWidget *
check(GtkWidget *box, const char *label, gboolean on, const char *tip)
{
    GtkWidget *c = gtk_check_button_new_with_mnemonic(label);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), on);
    gtk_widget_set_tooltip_text(c, tip);
    gtk_container_add(GTK_CONTAINER(box), c);
    return c;
}

static GtkWidget *
frame(const char *title, GtkWidget *child)
{
    GtkWidget *f = gtk_frame_new(title);

    gtk_container_set_border_width(GTK_CONTAINER(child), 6);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

/* Edits g in place.  Returns RESP_CONNECT, RESP_OFFLINE or 0. */
static int
login_dialog(GtkWindow *parent, Game *g, GList *games, gboolean is_new)
{
    GtkWidget *dlg, *content, *grid, *opts, *client, *hbox;
    LoginWidgets w;
    char port[16];
    int result = 0;

    dlg = gtk_dialog_new_with_buttons(
        "Empire Login", parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Offline", RESP_OFFLINE,
        "C_onnect", RESP_CONNECT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), RESP_CONNECT);
    gtk_widget_set_tooltip_text(
        gtk_dialog_get_widget_for_response(GTK_DIALOG(dlg), RESP_OFFLINE),
        "Look at the saved map and data without connecting");
    content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);
    gtk_box_set_spacing(GTK_BOX(content), 8);

    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    snprintf(port, sizeof(port), "%d", g->port);
    w.name = grid_entry(GTK_GRID(grid), 0, "_Game:", g->name);
    gtk_editable_set_editable(GTK_EDITABLE(w.name), is_new);
    gtk_widget_set_can_focus(w.name, is_new);
    w.host = grid_entry(GTK_GRID(grid), 1, "_Host name:", g->host);
    w.port = grid_entry(GTK_GRID(grid), 2, "_Port number:", port);
    w.country = grid_entry(GTK_GRID(grid), 3, "_Country name:", g->country);
    w.password = grid_entry(GTK_GRID(grid), 4, "Pass_word:", g->password);
    gtk_entry_set_visibility(GTK_ENTRY(w.password), FALSE);
    w.user = grid_entry(GTK_GRID(grid), 5, "_User:", g->user);
    gtk_container_add(GTK_CONTAINER(content), grid);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    w.kill = check(opts, "_Kill hung session", g->kill,
                   "Log out another connection that is still using this country");
    w.utf8 = check(opts, "UTF-_8", g->utf8,
                   "Ask the server to send UTF-8 text");
    gtk_box_pack_start(GTK_BOX(hbox), frame("Server Options", opts),
                       TRUE, TRUE, 0);
    client = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    w.deity = check(client, "Sign on as _Deity", g->deity,
                    "This country is a deity (game administrator)");
    w.autoread = check(client, "Auto_Read", g->autoread,
                       "Read telegrams as soon as they arrive");
    w.autoupdate = check(client, "Auto_Update", g->autoupdate,
                         "Refresh the map and database after logging in");
    gtk_box_pack_start(GTK_BOX(hbox), frame("Client Options", client),
                       TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(content), hbox);
    gtk_widget_show_all(dlg);
    gtk_widget_grab_focus(is_new ? w.name
                          : *g->country ? w.password : w.country);

    for (;;) {
        int resp = gtk_dialog_run(GTK_DIALOG(dlg));
        const char *name = gtk_entry_get_text(GTK_ENTRY(w.name));
        const char *host = gtk_entry_get_text(GTK_ENTRY(w.host));
        const char *cty = gtk_entry_get_text(GTK_ENTRY(w.country));
        char *end;
        long p = strtol(gtk_entry_get_text(GTK_ENTRY(w.port)), &end, 10);
        const char *err = NULL;
        char *trimmed;

        if (resp != RESP_CONNECT && resp != RESP_OFFLINE)
            break;
        trimmed = g_strstrip(g_strdup(name));
        if (!*trimmed || strchr(name, '[') || strchr(name, ']'))
            err = "Please enter a name for this game (without [ or ]).";
        else if (is_new && games_find(games, trimmed))
            err = "There is already a game with that name.";
        else if (resp == RESP_CONNECT && !*host)
            err = "Please enter the server's host name.";
        else if (resp == RESP_CONNECT && (*end || p <= 0 || p > 65535))
            err = "The port number must be between 1 and 65535.";
        else if (resp == RESP_CONNECT && !*cty)
            err = "Please enter your country name.";
        if (err) {
            GtkWidget *m = gtk_message_dialog_new(
                GTK_WINDOW(dlg), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                GTK_BUTTONS_OK, "%s", err);
            gtk_dialog_run(GTK_DIALOG(m));
            gtk_widget_destroy(m);
            g_free(trimmed);
            continue;
        }
        g_free(g->name);
        g->name = trimmed;
        g_free(g->host);
        g->host = g_strstrip(g_strdup(host));
        if (!*end && p > 0 && p <= 65535)
            g->port = p;
        g_free(g->country);
        g->country = g_strdup(cty);
        g_free(g->password);
        g->password = g_strdup(gtk_entry_get_text(GTK_ENTRY(w.password)));
        g_free(g->user);
        g->user = g_strdup(gtk_entry_get_text(GTK_ENTRY(w.user)));
#define TB(x) gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(x))
        g->kill = TB(w.kill);
        g->deity = TB(w.deity);
        g->autoupdate = TB(w.autoupdate);
        g->autoread = TB(w.autoread);
        g->utf8 = TB(w.utf8);
#undef TB
        result = resp;
        break;
    }
    gtk_widget_destroy(dlg);
    return result;
}

/* ------------------------------------------------------ game chooser */

enum { COL_NAME, COL_SERVER, COL_COUNTRY, NCOLS };
enum { RESP_NEW = 10, RESP_DELETE, RESP_OPEN };

static void
fill_store(GtkListStore *store, GList *games)
{
    gtk_list_store_clear(store);
    for (GList *l = games; l; l = l->next) {
        Game *g = l->data;
        char *server = g_strdup_printf("%s:%d", g->host, g->port);
        GtkTreeIter it;

        gtk_list_store_append(store, &it);
        gtk_list_store_set(store, &it, COL_NAME, g->name, COL_SERVER, server,
                           COL_COUNTRY, g->country, -1);
        g_free(server);
    }
}

static char *
selected_name(GtkTreeView *tv)
{
    GtkTreeModel *model;
    GtkTreeIter it;
    char *name = NULL;

    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(tv),
                                        &model, &it))
        gtk_tree_model_get(model, &it, COL_NAME, &name, -1);
    return name;
}

static void
row_activated(GtkTreeView *tv, GtkTreePath *p, GtkTreeViewColumn *c,
              gpointer dlg)
{
    gtk_dialog_response(GTK_DIALOG(dlg), RESP_OPEN);
}

static void
selection_changed(GtkTreeSelection *sel, gpointer dlg)
{
    gboolean any = gtk_tree_selection_count_selected_rows(sel) > 0;

    gtk_dialog_set_response_sensitive(GTK_DIALOG(dlg), RESP_OPEN, any);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dlg), RESP_DELETE, any);
}

Game *
game_choose(GtkWindow *parent, gboolean *connect)
{
    GtkWidget *dlg, *content, *sw, *tv, *label;
    GtkListStore *store;
    GtkCellRenderer *r;
    GList *games = games_load();
    Game *chosen = NULL;

    dlg = gtk_dialog_new_with_buttons(
        "Choose Empire Game", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Delete", RESP_DELETE, "_New Game", RESP_NEW,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", RESP_OPEN, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), RESP_OPEN);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 460, 300);
    content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);
    gtk_box_set_spacing(GTK_BOX(content), 6);

    label = gtk_label_new("Most recently played games:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_container_add(GTK_CONTAINER(content), label);

    store = gtk_list_store_new(NCOLS, G_TYPE_STRING, G_TYPE_STRING,
                               G_TYPE_STRING);
    tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    r = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tv), -1, "Game",
                                                r, "text", COL_NAME, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tv), -1,
                                                "Server", r, "text",
                                                COL_SERVER, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tv), -1,
                                                "Country", r, "text",
                                                COL_COUNTRY, NULL);
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(content), sw, TRUE, TRUE, 0);
    g_signal_connect(tv, "row-activated", G_CALLBACK(row_activated), dlg);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(tv)),
                     "changed", G_CALLBACK(selection_changed), dlg);
    fill_store(store, games);
    if (games) {
        GtkTreePath *p = gtk_tree_path_new_first();

        gtk_tree_selection_select_path(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(tv)), p);
        gtk_tree_path_free(p);
    }
    selection_changed(gtk_tree_view_get_selection(GTK_TREE_VIEW(tv)), dlg);
    gtk_widget_show_all(dlg);

    while (!chosen) {
        int resp = gtk_dialog_run(GTK_DIALOG(dlg));
        char *name = selected_name(GTK_TREE_VIEW(tv));
        GList *l = name ? games_find(games, name) : NULL;
        Game *g = NULL;
        int how;

        if (resp == RESP_DELETE && l) {
            GtkWidget *m = gtk_message_dialog_new(
                GTK_WINDOW(dlg), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                GTK_BUTTONS_YES_NO,
                "Remove the game \"%s\" from the list?\n\n"
                "Its saved map database is kept.", name);
            if (gtk_dialog_run(GTK_DIALOG(m)) == GTK_RESPONSE_YES) {
                game_free(l->data);
                games = g_list_delete_link(games, l);
                games_save(games);
                fill_store(store, games);
            }
            gtk_widget_destroy(m);
        } else if (resp == RESP_NEW || (resp == RESP_OPEN && l)) {
            gboolean is_new = resp == RESP_NEW;

            g = is_new ? game_new() : game_copy(l->data);
            how = login_dialog(GTK_WINDOW(dlg), g, games, is_new);
            if (how) {
                /* save edits and move to the front of the MRU list */
                if (l) {
                    game_free(l->data);
                    games = g_list_delete_link(games, l);
                }
                games = g_list_prepend(games, game_copy(g));
                games_save(games);
                *connect = how == RESP_CONNECT;
                chosen = g;
            } else {
                game_free(g);
                if (is_new)
                    fill_store(store, games);
            }
        } else if (resp != RESP_OPEN) {
            g_free(name);
            break;
        }
        g_free(name);
    }
    gtk_widget_destroy(dlg);
    games_free(games);
    return chosen;
}
