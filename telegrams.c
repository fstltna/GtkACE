/*
 * telegrams.c - the telegram window (WinACE frmTelegram, EmpTelegram).
 *
 * Telegrams, bulletins, production reports and announcements are taken
 * from the output of "read" and "wire" whenever those run, and kept in
 * the game database, so they stay available after the server deletes
 * them.  New telegrams and announcements are sent by answering the
 * server's line-by-line prompts.
 */
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sqlite3.h>
#include "gtkace.h"

#define MAXTELSIZE 1024         /* the server's limit, newlines included */

/* Folders, as shown in the window */
enum { F_ALL, F_TELE, F_BULL, F_PROD, F_ANNO, F_SENT, F_COUNT };
static const char *folder_names[F_COUNT] = {
    "All received", "Telegrams", "Bulletins", "Production reports",
    "Announcements", "Sent"
};
/* the server's names for kinds of message; "Sent" is ours */
static const char *folder_kind[F_COUNT] = {
    NULL, "Telegram", "BULLETIN", "Production Report", "Announcement", "Sent"
};

static struct {
    /* capture */
    gboolean reading;           /* output of read or wire */
    GString *body;              /* message being collected */
    char *kind, *sender, *dated;
    int from;
    int stored;                 /* new messages stored this command */
    /* window */
    GtkWidget *win, *folders, *list, *text, *status, *delete_chk;
    GtkListStore *fstore, *mstore;
    int folder;
} tg = { .folder = F_ALL };

enum { M_ID, M_UNREAD, M_WHO, M_KIND, M_DATED, M_PREVIEW, M_WEIGHT, M_NCOLS };

static gboolean
ensure_table(void)
{
    if (!db_is_open())
        return FALSE;
    sqlite3_exec(db_handle(),
                 "CREATE TABLE IF NOT EXISTS telegrams ("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT, "
                 "sender INTEGER, sender_name TEXT, recipient TEXT, "
                 "dated TEXT, received INTEGER, body TEXT, unread INTEGER, "
                 "UNIQUE (kind, sender, dated, body))",
                 NULL, NULL, NULL);
    return TRUE;
}

static void tele_window_refresh(void);

int
tele_unread(void)
{
    if (!ensure_table())
        return 0;
    return db_get_int("SELECT count(*) FROM telegrams WHERE unread", 0);
}

static void
show_unread_status(void)
{
    int n = tele_unread();
    char *t;

    if (!n) {
        status_set_mail(NULL);
        return;
    }
    t = g_strdup_printf("%d unread %s", n, n == 1 ? "message" : "messages");
    status_set_mail(t);
    g_free(t);
}

/* ---------------------------------------------------------- capture */

static gboolean
is_read_or_wire(const char *cmd)
{
    char word[16];
    size_t n;

    if (sscanf(cmd, " %15[a-zA-Z]", word) != 1)
        return FALSE;
    n = strlen(word);
    for (char *p = word; *p; p++)
        *p = g_ascii_tolower(*p);
    return n >= 3 && (!strncmp("read", word, n) || !strncmp("wire", word, n));
}

static void
finish_message(void)
{
    sqlite3_stmt *st;
    gsize len;

    if (!tg.kind)
        return;
    /* drop the blank lines between messages */
    len = tg.body->len;
    while (len && (tg.body->str[len - 1] == '\n' || tg.body->str[len - 1] == ' '))
        len--;
    g_string_truncate(tg.body, len);
    if (ensure_table()
        && sqlite3_prepare_v2(db_handle(), "INSERT OR IGNORE INTO telegrams "
                              "(kind, sender, sender_name, recipient, dated, "
                              "received, body, unread) VALUES (?,?,?,NULL,?,?,?,1)",
                              -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, tg.kind, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, tg.from);
        sqlite3_bind_text(st, 3, tg.sender ? tg.sender : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, tg.dated ? tg.dated : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, g_get_real_time() / G_USEC_PER_SEC);
        sqlite3_bind_text(st, 6, tg.body->str, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_DONE
            && sqlite3_changes(db_handle()) > 0)
            tg.stored++;
        sqlite3_finalize(st);
    }
    g_clear_pointer(&tg.kind, g_free);
    g_clear_pointer(&tg.sender, g_free);
    g_clear_pointer(&tg.dated, g_free);
    g_string_truncate(tg.body, 0);
}

/* "> Telegram from Name, (#2)  dated Sat Nov  7 21:04:22 2026"
 * "> Production Report  dated Sat Nov  7 21:04:22 2026" */
static gboolean
parse_header(const char *line)
{
    static const char *kinds[] = {
        "Telegram", "Announcement", "BULLETIN", "Production Report", NULL
    };
    const char *p = line + 2, *dated, *from;

    if (strncmp(line, "> ", 2) || !(dated = strstr(p, "  dated ")))
        return FALSE;
    for (int i = 0; kinds[i]; i++) {
        size_t n = strlen(kinds[i]);

        if (strncmp(p, kinds[i], n) || (p[n] != ' '))
            continue;
        finish_message();
        tg.kind = g_strdup(kinds[i]);
        tg.from = -1;
        from = p + n;
        if (g_str_has_prefix(from, " from ")) {
            const char *nat = strstr(from, ", (#");

            if (nat && nat < dated) {
                tg.sender = g_strndup(from + 6, nat - from - 6);
                tg.from = atoi(nat + 4);
            }
        }
        tg.dated = g_strdup(dated + 8);
        return TRUE;
    }
    return FALSE;
}

void
tele_command_start(const char *cmd)
{
    finish_message();
    tg.reading = is_read_or_wire(cmd);
    tg.stored = 0;
    if (!tg.body)
        tg.body = g_string_new(NULL);
}

void
tele_line(const char *text)
{
    if (!tg.reading)
        return;
    if (parse_header(text))
        return;
    if (!tg.kind)
        return;                 /* "Announcements since ...", "No ..." */
    if (g_str_has_prefix(text, "Wait a sec!  A new "))
        return;
    if (!tg.body->len && !*text)
        return;
    g_string_append(tg.body, text);
    g_string_append_c(tg.body, '\n');
}

void
tele_command_end(void)
{
    if (!tg.reading)
        return;
    finish_message();
    tg.reading = FALSE;
    show_unread_status();
    if (tg.stored)
        tele_window_refresh();
}

/* Fetch telegrams and announcements into the store.  The answer to
 * "Shall I burn it?" is given with the command. */
void
tele_check(void)
{
    char *d = db_get_setting("tele_delete");
    gboolean del = !d || atoi(d);

    g_free(d);
    conn_command(del ? "read y" : "read n", CMD_SHOW, NULL, NULL, NULL);
    conn_command(del ? "wire y" : "wire n", CMD_SHOW, NULL, NULL, NULL);
}

void
tele_autoread(void)
{
    char *d = db_get_setting("tele_delete");
    gboolean del = !d || atoi(d);

    g_free(d);
    conn_command(del ? "read y" : "read n", CMD_SHOW, NULL, NULL, NULL);
}

/* ------------------------------------------------------------ window */

/* Folder rows are made once; after that only their labels change, so
 * the selection (the folder being shown) is never disturbed. */
static void
fill_folders(void)
{
    GtkTreeIter it;
    gboolean create;

    if (!tg.fstore)
        return;
    create = !gtk_tree_model_get_iter_first(GTK_TREE_MODEL(tg.fstore), &it);
    for (int f = 0; f < F_COUNT; f++) {
        char *sql, *label;
        int unread;

        if (f == F_ALL)
            sql = g_strdup("SELECT count(*) FROM telegrams WHERE unread AND "
                           "kind <> 'Sent'");
        else
            sql = g_strdup_printf("SELECT count(*) FROM telegrams WHERE unread "
                                  "AND kind = '%s'", folder_kind[f]);
        unread = db_get_int(sql, 0);
        g_free(sql);
        label = unread ? g_strdup_printf("%s (%d)", folder_names[f], unread)
            : g_strdup(folder_names[f]);
        if (create)
            gtk_list_store_append(tg.fstore, &it);
        gtk_list_store_set(tg.fstore, &it, 0, label, 1, f,
                           2, unread ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                           -1);
        g_free(label);
        if (!create && !gtk_tree_model_iter_next(GTK_TREE_MODEL(tg.fstore), &it))
            break;
    }
    if (create && gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(tg.fstore), &it,
                                                NULL, tg.folder))
        gtk_tree_selection_select_iter(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(tg.folders)), &it);
}

static void message_selected(GtkTreeSelection *sel, gpointer d);

static void
fill_messages(void)
{
    GtkTreeSelection *msel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tg.list));
    sqlite3_stmt *st;
    char *sql;
    GtkTreeIter it;
    int n = 0, first_unread = -1;

    /* Clearing a list moves the selection along row by row as rows go,
     * which would open (and mark read) every message: keep quiet */
    g_signal_handlers_block_by_func(msel, message_selected, NULL);
    gtk_tree_selection_unselect_all(msel);
    gtk_list_store_clear(tg.mstore);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tg.text)),
                             "", -1);
    if (!ensure_table()) {
        g_signal_handlers_unblock_by_func(msel, message_selected, NULL);
        return;
    }
    if (tg.folder == F_ALL)
        sql = g_strdup("SELECT id, unread, sender_name, sender, recipient, kind, "
                       "dated, body FROM telegrams WHERE kind <> 'Sent' "
                       "ORDER BY id");
    else
        sql = g_strdup_printf("SELECT id, unread, sender_name, sender, recipient, "
                              "kind, dated, body FROM telegrams WHERE kind = '%s' "
                              "ORDER BY id", folder_kind[tg.folder]);
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 2);
            const char *to = (const char *)sqlite3_column_text(st, 4);
            const char *kind = (const char *)sqlite3_column_text(st, 5);
            const char *body = (const char *)sqlite3_column_text(st, 7);
            gboolean unread = sqlite3_column_int(st, 1);
            char *who, *preview, *nl;

            if (kind && !strcmp(kind, "Sent"))
                who = g_strdup_printf("to %s", to ? to : "?");
            else if (name && *name)
                who = g_strdup(name);
            else
                who = g_strdup("the server");
            preview = g_strdup(body ? body : "");
            if ((nl = strchr(preview, '\n')))
                *nl = 0;
            gtk_list_store_append(tg.mstore, &it);
            gtk_list_store_set(tg.mstore, &it, M_ID, sqlite3_column_int(st, 0),
                               M_UNREAD, unread ? "\342\200\242" : "",
                               M_WHO, who, M_KIND, kind,
                               M_DATED, sqlite3_column_text(st, 6),
                               M_PREVIEW, preview,
                               M_WEIGHT, unread ? PANGO_WEIGHT_BOLD
                               : PANGO_WEIGHT_NORMAL, -1);
            g_free(who);
            g_free(preview);
            if (unread && first_unread < 0)
                first_unread = n;
            n++;
        }
        sqlite3_finalize(st);
    }
    g_free(sql);
    {
        char *t = g_strdup_printf("%d %s", n, n == 1 ? "message" : "messages");

        gtk_label_set_text(GTK_LABEL(tg.status), t);
        g_free(t);
    }
    g_signal_handlers_unblock_by_func(msel, message_selected, NULL);
    /* show the oldest unread message, or else the newest */
    if (n) {
        GtkTreePath *p = gtk_tree_path_new_from_indices(
            first_unread >= 0 ? first_unread : n - 1, -1);

        gtk_tree_view_set_cursor(GTK_TREE_VIEW(tg.list), p, NULL, FALSE);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(tg.list), p, NULL, FALSE,
                                     0, 0);
        gtk_tree_path_free(p);
    }
}

static void
tele_window_refresh(void)
{
    if (!tg.win)
        return;
    fill_folders();
    fill_messages();
}

static int
selected_id(GtkTreeIter *itp)
{
    GtkTreeModel *m;
    GtkTreeIter it;
    int id;

    if (!tg.list || !gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(tg.list)), &m, &it))
        return -1;
    gtk_tree_model_get(m, &it, M_ID, &id, -1);
    if (itp)
        *itp = it;
    return id;
}

/* Fields of a stored message (newly allocated, NULL if none) */
static gboolean
get_message(int id, char **kind, char **who, int *from, char **dated,
            char **body)
{
    sqlite3_stmt *st;
    gboolean ok = FALSE;

    if (sqlite3_prepare_v2(db_handle(), "SELECT kind, coalesce(sender_name, "
                           "recipient), sender, dated, body FROM telegrams "
                           "WHERE id=?", -1, &st, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_int(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *kind = g_strdup((const char *)sqlite3_column_text(st, 0));
        *who = g_strdup((const char *)sqlite3_column_text(st, 1));
        *from = sqlite3_column_int(st, 2);
        *dated = g_strdup((const char *)sqlite3_column_text(st, 3));
        *body = g_strdup((const char *)sqlite3_column_text(st, 4));
        ok = TRUE;
    }
    sqlite3_finalize(st);
    return ok;
}

static void
message_selected(GtkTreeSelection *sel, gpointer d)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tg.text));
    GtkTreeIter it;
    char *kind, *who, *dated, *body, *head;
    int from, id = selected_id(&it);

    if (id < 0 || !get_message(id, &kind, &who, &from, &dated, &body)) {
        gtk_text_buffer_set_text(buf, "", -1);
        return;
    }
    if (!strcmp(kind, "Sent"))
        head = g_strdup_printf("Sent to %s, %s\n\n", who ? who : "?", dated);
    else if (who && *who)
        head = g_strdup_printf("%s from %s (#%d), dated %s\n\n", kind, who, from,
                               dated);
    else
        head = g_strdup_printf("%s, dated %s\n\n", kind, dated);
    gtk_text_buffer_set_text(buf, "", -1);
    {
        GtkTextIter end;

        gtk_text_buffer_get_end_iter(buf, &end);
        gtk_text_buffer_insert_with_tags_by_name(buf, &end, head, -1, "head",
                                                 NULL);
        gtk_text_buffer_get_end_iter(buf, &end);
        gtk_text_buffer_insert(buf, &end, body ? body : "", -1);
    }
    /* mark read */
    {
        char *sql = g_strdup_printf("UPDATE telegrams SET unread=0 WHERE id=%d "
                                    "AND unread", id);

        sqlite3_exec(db_handle(), sql, NULL, NULL, NULL);
        g_free(sql);
        if (sqlite3_changes(db_handle()) > 0) {
            gtk_list_store_set(tg.mstore, &it, M_UNREAD, "",
                               M_WEIGHT, PANGO_WEIGHT_NORMAL, -1);
            fill_folders();
            show_unread_status();
        }
    }
    g_free(head);
    g_free(kind);
    g_free(who);
    g_free(dated);
    g_free(body);
}

static void
folder_selected(GtkTreeSelection *sel, gpointer d)
{
    GtkTreeModel *m;
    GtkTreeIter it;
    int f;

    if (!gtk_tree_selection_get_selected(sel, &m, &it))
        return;
    gtk_tree_model_get(m, &it, 1, &f, -1);
    if (f != tg.folder) {
        tg.folder = f;
        fill_messages();
    }
}

static void
move_selection(int delta)
{
    GtkTreeIter it;
    GtkTreePath *p;
    int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(tg.mstore), NULL);
    int i;

    if (!n)
        return;
    if (selected_id(&it) < 0) {
        i = delta < 0 ? n - 1 : 0;
    } else {
        p = gtk_tree_model_get_path(GTK_TREE_MODEL(tg.mstore), &it);
        i = gtk_tree_path_get_indices(p)[0] + delta;
        gtk_tree_path_free(p);
    }
    i = CLAMP(i, 0, n - 1);
    p = gtk_tree_path_new_from_indices(i, -1);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(tg.list), p, NULL, FALSE);
    gtk_tree_path_free(p);
}

static void act_compose(const char *to, const char *body);

static void
on_check(GtkWidget *w, gpointer d)
{
    if (conn_state() != CONN_PLAYING) {
        mainwin_error("Not connected",
                      "Connect to the server to check for new telegrams.");
        return;
    }
    tele_check();
}

static void
on_new(GtkWidget *w, gpointer d)
{
    act_compose(NULL, NULL);
}

static void
on_reply(GtkWidget *w, gpointer forward)
{
    char *kind, *who, *dated, *body;
    int from, id = selected_id(NULL);
    GString *q;

    if (id < 0 || !get_message(id, &kind, &who, &from, &dated, &body))
        return;
    q = g_string_new(NULL);
    if (forward) {
        g_string_printf(q, "Forwarded %s%s%s, dated %s:\n", kind,
                        who && *who ? " from " : "", who && *who ? who : "",
                        dated);
        g_string_append(q, body ? body : "");
        act_compose(NULL, q->str);
    } else {
        /* quote the original, as WinACE's "include text" option did */
        char **lines = g_strsplit(body ? body : "", "\n", -1);

        g_string_append(q, "\n\n");
        for (int i = 0; lines[i]; i++)
            g_string_append_printf(q, "| %s\n", lines[i]);
        g_strfreev(lines);
        act_compose(!strcmp(kind, "Sent") || !who || !*who ? NULL : who,
                    q->str);
    }
    g_string_free(q, TRUE);
    g_free(kind);
    g_free(who);
    g_free(dated);
    g_free(body);
}

static void
on_delete(GtkWidget *w, gpointer d)
{
    GtkTreeIter it;
    int id = selected_id(&it);
    char *sql;
    GtkTreePath *p;
    int idx;

    if (id < 0)
        return;
    p = gtk_tree_model_get_path(GTK_TREE_MODEL(tg.mstore), &it);
    idx = gtk_tree_path_get_indices(p)[0];
    gtk_tree_path_free(p);
    sql = g_strdup_printf("DELETE FROM telegrams WHERE id=%d", id);
    sqlite3_exec(db_handle(), sql, NULL, NULL, NULL);
    g_free(sql);
    fill_folders();
    fill_messages();
    show_unread_status();
    /* keep the place in the list */
    if (gtk_tree_model_iter_n_children(GTK_TREE_MODEL(tg.mstore), NULL)) {
        int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(tg.mstore), NULL);

        p = gtk_tree_path_new_from_indices(MIN(idx, n - 1), -1);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(tg.list), p, NULL, FALSE);
        gtk_tree_path_free(p);
    }
}

static void
on_prev(GtkWidget *w, gpointer d)
{
    move_selection(-1);
}

static void
on_next(GtkWidget *w, gpointer d)
{
    move_selection(1);
}

static void
delete_toggled(GtkToggleButton *b, gpointer d)
{
    db_set_setting("tele_delete", gtk_toggle_button_get_active(b) ? "1" : "0");
}

static gboolean
key_press(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    if (ev->keyval == GDK_KEY_Escape) {
        gtk_widget_destroy(w);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Delete
        && gtk_widget_has_focus(tg.list)) {
        on_delete(NULL, NULL);
        return TRUE;
    }
    return FALSE;
}

static void
destroyed(GtkWidget *w, gpointer d)
{
    tg.win = tg.folders = tg.list = tg.text = tg.status = tg.delete_chk = NULL;
    tg.fstore = tg.mstore = NULL;
}

static GtkToolItem *
tool(GtkWidget *bar, const char *label, const char *tip, GCallback cb,
     gpointer data)
{
    GtkToolItem *b = gtk_tool_button_new(NULL, label);

    gtk_tool_button_set_use_underline(GTK_TOOL_BUTTON(b), TRUE);
    gtk_tool_item_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, data);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), b, -1);
    return b;
}

void
tele_window_show(void)
{
    GtkWidget *box, *bar, *hp, *vp, *sw, *bottom;
    GtkCellRenderer *r;
    GtkTreeViewColumn *col;
    char *d;

    if (tg.win) {
        gtk_window_present(GTK_WINDOW(tg.win));
        return;
    }
    ensure_table();
    tg.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(tg.win), "Telegrams");
    gtk_window_set_transient_for(GTK_WINDOW(tg.win), mainwin_window());
    gtk_window_set_default_size(GTK_WINDOW(tg.win), 820, 560);
    g_signal_connect(tg.win, "destroy", G_CALLBACK(destroyed), NULL);
    g_signal_connect(tg.win, "key-press-event", G_CALLBACK(key_press), NULL);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(tg.win), box);

    bar = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(bar), GTK_TOOLBAR_TEXT);
    tool(bar, "_Check for New", "Read new telegrams and announcements from the "
         "server", G_CALLBACK(on_check), NULL);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), gtk_separator_tool_item_new(), -1);
    tool(bar, "_New", "Write a telegram or announcement", G_CALLBACK(on_new),
         NULL);
    tool(bar, "_Reply", "Reply to the sender", G_CALLBACK(on_reply), NULL);
    tool(bar, "_Forward", "Send this message on to someone else",
         G_CALLBACK(on_reply), GINT_TO_POINTER(1));
    tool(bar, "_Delete", "Delete this message from GtkACE (Delete key)",
         G_CALLBACK(on_delete), NULL);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), gtk_separator_tool_item_new(), -1);
    tool(bar, "_Previous", "Previous message", G_CALLBACK(on_prev), NULL);
    tool(bar, "Ne_xt", "Next message", G_CALLBACK(on_next), NULL);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

    hp = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    tg.fstore = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT);
    tg.folders = gtk_tree_view_new_with_model(GTK_TREE_MODEL(tg.fstore));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tg.folders), FALSE);
    r = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tg.folders), -1,
                                                "Folder", r, "text", 0,
                                                "weight", 2, NULL);
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), tg.folders);
    gtk_widget_set_size_request(sw, 170, -1);
    gtk_paned_pack1(GTK_PANED(hp), sw, FALSE, FALSE);

    vp = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    tg.mstore = gtk_list_store_new(M_NCOLS, G_TYPE_INT, G_TYPE_STRING,
                                   G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                   G_TYPE_STRING, G_TYPE_INT);
    tg.list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(tg.mstore));
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tg.list), FALSE);
    {
        static const struct { const char *t; int c; } cols[] = {
            { "", M_UNREAD }, { "From", M_WHO }, { "Kind", M_KIND },
            { "Dated", M_DATED }, { "First line", M_PREVIEW }
        };
        for (guint i = 0; i < G_N_ELEMENTS(cols); i++) {
            r = gtk_cell_renderer_text_new();
            if (cols[i].c == M_PREVIEW)
                g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
            col = gtk_tree_view_column_new_with_attributes(cols[i].t, r, "text",
                                                           cols[i].c, "weight",
                                                           M_WEIGHT, NULL);
            gtk_tree_view_column_set_resizable(col, TRUE);
            gtk_tree_view_column_set_expand(col, cols[i].c == M_PREVIEW);
            gtk_tree_view_append_column(GTK_TREE_VIEW(tg.list), col);
        }
    }
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), tg.list);
    gtk_widget_set_size_request(sw, -1, 150);
    gtk_paned_pack1(GTK_PANED(vp), sw, FALSE, FALSE);

    tg.text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tg.text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tg.text), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tg.text), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tg.text), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tg.text), 4);
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tg.text)),
                               "head", "weight", PANGO_WEIGHT_BOLD,
                               "foreground", "#1a4fb5", NULL);
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), tg.text);
    gtk_paned_pack2(GTK_PANED(vp), sw, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(vp), 200);
    gtk_paned_pack2(GTK_PANED(hp), vp, TRUE, FALSE);
    gtk_box_pack_start(GTK_BOX(box), hp, TRUE, TRUE, 0);

    bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(bottom), 4);
    tg.status = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(bottom), tg.status, FALSE, FALSE, 4);
    tg.delete_chk = gtk_check_button_new_with_mnemonic(
        "D_elete telegrams from the server once GtkACE has them");
    d = db_get_setting("tele_delete");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tg.delete_chk),
                                 !d || atoi(d));
    g_free(d);
    g_signal_connect(tg.delete_chk, "toggled", G_CALLBACK(delete_toggled), NULL);
    gtk_box_pack_end(GTK_BOX(bottom), tg.delete_chk, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 0);

    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(tg.folders)),
                     "changed", G_CALLBACK(folder_selected), NULL);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(tg.list)),
                     "changed", G_CALLBACK(message_selected), NULL);
    fill_folders();
    fill_messages();
    gtk_widget_show_all(tg.win);
}

/* ------------------------------------------------------------ compose */

static struct {
    GtkWidget *win, *to, *text, *count, *save, *send;
} cp;

/* What the server will count: each line plus its newline */
static int
tele_size(const char *body, int *lines)
{
    char **l = g_strsplit(body, "\n", -1);
    int n = 0, i;

    for (i = 0; l[i]; i++)
        n += strlen(l[i]) + 1;
    /* a trailing newline gives an empty last line we don't send */
    if (i && !*l[i - 1]) {
        i--;
        n--;
    }
    g_strfreev(l);
    if (lines)
        *lines = i;
    return n;
}

static char *
compose_body(void)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(cp.text));
    GtkTextIter s, e;

    gtk_text_buffer_get_bounds(b, &s, &e);
    return gtk_text_buffer_get_text(b, &s, &e, FALSE);
}

static void
compose_update(void)
{
    char *body = compose_body(), *t;
    int n = tele_size(body, NULL);
    const char *to = gtk_entry_get_text(GTK_ENTRY(
        gtk_bin_get_child(GTK_BIN(cp.to))));

    t = g_strdup_printf("%d of %d characters", n, MAXTELSIZE);
    gtk_label_set_text(GTK_LABEL(cp.count), t);
    g_free(t);
    gtk_widget_set_sensitive(cp.send, *body && *to && n <= MAXTELSIZE
                             && conn_state() == CONN_PLAYING);
    g_free(body);
}

#define EVERYONE "Everyone (announcement)"

static void
compose_send(GtkWidget *w, gpointer d)
{
    const char *to = gtk_entry_get_text(GTK_ENTRY(
        gtk_bin_get_child(GTK_BIN(cp.to))));
    char *body = compose_body(), *cmd, *name;
    char **lines;
    GPtrArray *answers = g_ptr_array_new_with_free_func(g_free);
    gboolean anno = !strcmp(to, EVERYONE);
    int nlines;

    tele_size(body, &nlines);
    lines = g_strsplit(body, "\n", -1);
    for (int i = 0; i < nlines; i++) {
        const char *l = lines[i];

        /* the server ends a telegram at ".", takes "~x" as an editing
         * command and an empty line as giving up: keep them as text */
        if (!*l || !strcmp(l, ".") || (l[0] == '~' && l[1] && !l[2]))
            g_ptr_array_add(answers, g_strconcat(" ", l, NULL));
        else
            g_ptr_array_add(answers, g_strdup(l));
    }
    g_ptr_array_add(answers, g_strdup("."));
    g_ptr_array_add(answers, NULL);
    g_strfreev(lines);

    /* a country name in quotes, so names with spaces work */
    name = g_strstrip(g_strdup(to));
    for (char *p = name; *p; p++)
        if (*p == '"')
            *p = '\'';
    cmd = anno ? g_strdup("announce") : g_strdup_printf("telegram \"%s\"", name);
    conn_command_answers(cmd, CMD_SHOW | CMD_USER,
                         (const char *const *)answers->pdata, NULL, NULL);
    g_free(cmd);
    g_ptr_array_unref(answers);

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cp.save))
        && ensure_table()) {
        sqlite3_stmt *st;
        GDateTime *now = g_date_time_new_now_local();
        char *dated = g_date_time_format(now, "%a %b %e %H:%M:%S %Y");

        if (sqlite3_prepare_v2(db_handle(), "INSERT OR IGNORE INTO telegrams "
                               "(kind, sender, recipient, dated, received, body, "
                               "unread) VALUES ('Sent', ?, ?, ?, ?, ?, 0)", -1,
                               &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, my_cnum);
            sqlite3_bind_text(st, 2, anno ? "everyone" : name, -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, dated, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 4, g_get_real_time() / G_USEC_PER_SEC);
            sqlite3_bind_text(st, 5, body, -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        g_free(dated);
        g_date_time_unref(now);
        tele_window_refresh();
    }
    g_free(name);
    g_free(body);
    gtk_widget_destroy(cp.win);
}

static void
compose_destroyed(GtkWidget *w, gpointer d)
{
    memset(&cp, 0, sizeof(cp));
}

static void
act_compose(const char *to, const char *body)
{
    GtkWidget *box, *grid, *sw, *bb, *cancel, *l;
    sqlite3_stmt *st;

    if (cp.win) {
        gtk_window_present(GTK_WINDOW(cp.win));
        return;
    }
    cp.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(cp.win), "New Telegram");
    gtk_window_set_transient_for(GTK_WINDOW(cp.win),
                                 tg.win ? GTK_WINDOW(tg.win) : mainwin_window());
    gtk_window_set_default_size(GTK_WINDOW(cp.win), 560, 380);
    g_signal_connect(cp.win, "destroy", G_CALLBACK(compose_destroyed), NULL);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(cp.win), box);

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    l = gtk_label_new_with_mnemonic("_To:");
    cp.to = gtk_combo_box_text_new_with_entry();
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), cp.to);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cp.to), EVERYONE);
    if (db_table_exists("nat")
        && sqlite3_prepare_v2(db_handle(), "SELECT cnum, cname FROM nat "
                              "ORDER BY cname", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
            if (sqlite3_column_int(st, 0) != my_cnum)
                gtk_combo_box_text_append_text(
                    GTK_COMBO_BOX_TEXT(cp.to),
                    (const char *)sqlite3_column_text(st, 1));
        sqlite3_finalize(st);
    }
    gtk_widget_set_hexpand(cp.to, TRUE);
    gtk_grid_attach(GTK_GRID(grid), l, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), cp.to, 1, 0, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    cp.text = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(cp.text), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(cp.text), GTK_WRAP_NONE);
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), cp.text);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);

    bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    cp.count = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(bb), cp.count, FALSE, FALSE, 0);
    cp.save = gtk_check_button_new_with_mnemonic("_Keep a copy in Sent");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cp.save), TRUE);
    gtk_box_pack_start(GTK_BOX(bb), cp.save, FALSE, FALSE, 8);
    cp.send = gtk_button_new_with_mnemonic("_Send");
    cancel = gtk_button_new_with_mnemonic("_Cancel");
    gtk_box_pack_end(GTK_BOX(bb), cp.send, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bb), cancel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bb, FALSE, FALSE, 0);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_widget_destroy),
                             cp.win);
    g_signal_connect(cp.send, "clicked", G_CALLBACK(compose_send), NULL);
    g_signal_connect_swapped(gtk_text_view_get_buffer(GTK_TEXT_VIEW(cp.text)),
                             "changed", G_CALLBACK(compose_update), NULL);
    g_signal_connect_swapped(cp.to, "changed", G_CALLBACK(compose_update),
                             NULL);

    if (to)
        gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(cp.to))), to);
    if (body) {
        GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(cp.text));
        GtkTextIter start;

        gtk_text_buffer_set_text(b, body, -1);
        gtk_text_buffer_get_start_iter(b, &start);
        gtk_text_buffer_place_cursor(b, &start);
    }
    compose_update();
    gtk_widget_show_all(cp.win);
    gtk_widget_grab_focus(to ? cp.text : cp.to);
}
