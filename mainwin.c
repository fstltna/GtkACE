/*
 * mainwin.c - the main window (WinACE frmDrawMap): menus, toolbar,
 * map, sector panel, server output, command line and status bar.
 */
#include <string.h>
#include <stdlib.h>
#include <sqlite3.h>
#include "gtkace.h"

#define MAX_OUTPUT_LINES 20000
#define HISTORY_MAX 200

static struct {
    GtkWidget *win;
    GtkWidget *output;          /* GtkTextView */
    GtkTextBuffer *buf;
    GtkTextMark *end;
    GtkWidget *entry, *entry_label;
    GtkWidget *st_conn, *st_time, *st_mail, *st_busy, *st_hover;
    GtkWidget *panel_title, *panel_grid, *panel_units;
    GHashTable *panel_vals;     /* field key -> GtkLabel */
    GList *needs_conn;          /* widgets usable only when connected */
    GtkWidget *connect_item;
    Game *game;
    GPtrArray *history;
    int hist_pos;
    char *hist_draft;
    gboolean refreshing;
} mw;

GtkWindow *
mainwin_window(void)
{
    return GTK_WINDOW(mw.win);
}

/* ------------------------------------------------------------ text */

char *
emp_text_decode(const char *raw, gboolean utf8, int **hl)
{
    GString *s = g_string_new(NULL);
    GArray *runs = g_array_new(FALSE, FALSE, sizeof(int));
    gboolean on = FALSE;
    int col = 0;
    char *valid;

    for (const unsigned char *p = (const unsigned char *)raw; *p; p++) {
        unsigned char c = *p;
        gboolean high = FALSE;

        if (!utf8 && c >= 0x80) {
            /* ASCII session: the top bit marks highlighted text */
            c &= 0x7f;
            high = TRUE;
        } else if (utf8 && (c == 14 || c == 15)) {
            high = c == 14;
            if (high != on) {
                int off = s->len;
                g_array_append_val(runs, off);
                on = high;
            }
            continue;
        }
        if (!utf8 && high != on) {
            int off = s->len;
            g_array_append_val(runs, off);
            on = high;
        }
        if (c == '\t') {
            do {
                g_string_append_c(s, ' ');
                col++;
            } while (col % 8);
            continue;
        }
        if (c == 7) {
            /* ring only for text being displayed (hl is wanted then) */
            if (mw.win && hl)
                gdk_window_beep(gtk_widget_get_window(mw.win));
            continue;
        }
        if (c < 32 || c == 127)
            continue;
        g_string_append_c(s, c);
        /* count characters, not UTF-8 continuation bytes */
        if ((c & 0xc0) != 0x80)
            col++;
    }
    if (on) {
        int off = s->len;
        g_array_append_val(runs, off);
    }
    valid = g_utf8_make_valid(s->str, s->len);
    if (strlen(valid) != s->len) {
        /* replacement characters moved the offsets: drop highlighting */
        g_array_set_size(runs, 0);
    }
    g_string_free(s, TRUE);
    if (hl) {
        if (runs->len) {
            int end = -1;
            g_array_append_val(runs, end);
            *hl = (int *)g_array_free(runs, FALSE);
            return valid;
        }
        *hl = NULL;
    }
    g_array_free(runs, TRUE);
    return valid;
}

static void
trim_output(void)
{
    int lines = gtk_text_buffer_get_line_count(mw.buf);

    if (lines > MAX_OUTPUT_LINES) {
        GtkTextIter a, b;

        gtk_text_buffer_get_start_iter(mw.buf, &a);
        gtk_text_buffer_get_iter_at_line(mw.buf, &b,
                                         lines - MAX_OUTPUT_LINES + 1000);
        gtk_text_buffer_delete(mw.buf, &a, &b);
    }
}

static void
append(const char *text, int *hl, const char *tag)
{
    GtkTextIter it;
    int base;

    if (!mw.buf) {
        g_printerr("%s\n", text);
        return;
    }
    gtk_text_buffer_get_end_iter(mw.buf, &it);
    base = gtk_text_iter_get_offset(&it);
    if (tag)
        gtk_text_buffer_insert_with_tags_by_name(mw.buf, &it, text, -1, tag,
                                                 NULL);
    else
        gtk_text_buffer_insert(mw.buf, &it, text, -1);
    if (hl) {
        for (int i = 0; hl[i] >= 0 && hl[i + 1] >= 0; i += 2) {
            GtkTextIter a, b;
            /* offsets are bytes; convert to characters */
            long ca = g_utf8_pointer_to_offset(text, text + hl[i]);
            long cb = g_utf8_pointer_to_offset(text, text + hl[i + 1]);

            gtk_text_buffer_get_iter_at_offset(mw.buf, &a, base + ca);
            gtk_text_buffer_get_iter_at_offset(mw.buf, &b, base + cb);
            gtk_text_buffer_apply_tag_by_name(mw.buf, "highlight", &a, &b);
        }
    }
    gtk_text_buffer_get_end_iter(mw.buf, &it);
    gtk_text_buffer_insert(mw.buf, &it, "\n", 1);
    trim_output();
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(mw.output), mw.end);
}

void
out_text(const char *text, const char *tag)
{
    char *t = g_utf8_make_valid(text, -1);

    append(t, NULL, tag);
    g_free(t);
}

void
out_server_line(int id, const char *text, gboolean utf8)
{
    int *hl = NULL;
    char *t = emp_text_decode(text, utf8, &hl);
    const char *tag = NULL;

    switch (id) {
    case C_FLASH: tag = "flash"; break;
    case C_CMDERR: case C_BADCMD: tag = "error"; break;
    case C_ABORT: tag = "notice"; break;
    default: break;
    }
    append(t, hl, tag);
    g_free(hl);
    g_free(t);
}

static int last_min = -1, last_btu = -1;

void
out_prompt(const char *cmd)
{
    char *line;

    if (last_btu >= 0)
        line = g_strdup_printf("[%d:%d] Command : %s", last_min, last_btu, cmd);
    else
        line = g_strdup_printf("Command : %s", cmd);
    out_text(line, "command");
    g_free(line);
}

/* --------------------------------------------------------- status */

void
status_set_conn(ConnState st, const char *detail)
{
    static const char *names[] = { "Offline", "Connecting", "Logging in",
                                   "Connected" };
    char *t;

    if (!mw.st_conn)
        return;
    t = g_strdup_printf(" %s ", names[st]);
    gtk_label_set_text(GTK_LABEL(mw.st_conn), t);
    g_free(t);
    if (detail)
        gtk_widget_set_tooltip_text(mw.st_conn, detail);
    for (GList *l = mw.needs_conn; l; l = l->next)
        gtk_widget_set_sensitive(l->data, st == CONN_PLAYING);
    if (mw.connect_item)
        gtk_widget_set_sensitive(mw.connect_item,
                                 st == CONN_DOWN && mw.game != NULL);
    if (st != CONN_PLAYING) {
        gtk_label_set_text(GTK_LABEL(mw.st_time), "");
        last_min = last_btu = -1;
    }
}

void
status_set_btu(int minutes, int btu)
{
    char t[64];

    last_min = minutes;
    last_btu = btu;
    snprintf(t, sizeof(t), " BTU %d  Time %d ", btu, minutes);
    gtk_label_set_text(GTK_LABEL(mw.st_time), t);
}

void
status_set_mail(const char *text)
{
    gtk_label_set_text(GTK_LABEL(mw.st_mail), text && *text ? text : "");
}

void
status_set_busy(int queued)
{
    char t[32];

    if (queued > 0)
        snprintf(t, sizeof(t), " Busy (%d) ", queued);
    else
        t[0] = 0;
    gtk_label_set_text(GTK_LABEL(mw.st_busy), t);
}

void
status_set_question(const char *q)
{
    if (q) {
        int *hl = NULL;
        char *t = emp_text_decode(q, TRUE, &hl);

        g_free(hl);
        gtk_label_set_text(GTK_LABEL(mw.entry_label), t);
        gtk_style_context_add_class(gtk_widget_get_style_context(
                                        mw.entry_label), "question");
        append(t, NULL, "question");
        g_free(t);
        gtk_widget_grab_focus(mw.entry);
    } else {
        gtk_label_set_text(GTK_LABEL(mw.entry_label), "Command:");
        gtk_style_context_remove_class(gtk_widget_get_style_context(
                                           mw.entry_label), "question");
    }
}

void
status_hover(int x, int y, gboolean valid)
{
    char t[96];
    Sector *s;

    if (!valid) {
        gtk_label_set_text(GTK_LABEL(mw.st_hover), "");
        return;
    }
    s = world_sector(x, y);
    if (s && s->des)
        snprintf(t, sizeof(t), " %d,%d  %s ", x, y,
                 *world_des_name(s->des) ? world_des_name(s->des) : "?");
    else
        snprintf(t, sizeof(t), " %d,%d ", x, y);
    gtk_label_set_text(GTK_LABEL(mw.st_hover), t);
}

void
mainwin_error(const char *title, const char *msg)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(mw.win),
                                          GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                          "%s", title);

    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", msg);
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show(d);
}

/* ---------------------------------------------------- sector panel */

typedef struct {
    const char *key;            /* column name, or a special key */
    const char *label;
} PanelField;

static const PanelField panel_general[] = {
    { "owner", "Owner" }, { "effic", "Efficiency" }, { "mobil", "Mobility" },
    { "work", "Work" }, { "avail", "Avail" }, { "terr0", "Territory" },
    { "road", "Road" }, { "rail", "Rail" }, { "dfense", "Defense" },
    { "dist", "Dist sector" },
};

static const PanelField panel_res[] = {
    { "min", "Min" }, { "gold", "Gold" }, { "fert", "Fert" },
    { "ocontent", "Oil" }, { "uran", "Uran" },
};

/* item column, threshold column, label */
static const char *panel_items[][3] = {
    { "civil", "c_dist", "Civ" }, { "milit", "m_dist", "Mil" },
    { "uw", "u_dist", "Uw" }, { "food", "f_dist", "Food" },
    { "shell", "s_dist", "Shell" }, { "gun", "g_dist", "Guns" },
    { "petrol", "p_dist", "Pet" }, { "iron", "i_dist", "Iron" },
    { "dust", "d_dist", "Dust" }, { "bar", "b_dist", "Bars" },
    { "oil", "o_dist", "Oil" }, { "lcm", "l_dist", "Lcms" },
    { "hcm", "h_dist", "Hcms" }, { "rad", "r_dist", "Rads" },
};

static GtkWidget *
value_label(const char *key)
{
    GtkWidget *l = gtk_label_new("");

    gtk_label_set_xalign(GTK_LABEL(l), 1.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "value");
    g_hash_table_insert(mw.panel_vals, g_strdup(key), l);
    return l;
}

static GtkWidget *
name_label(const char *text, gboolean bold)
{
    GtkWidget *l = gtk_label_new(text);

    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    if (bold)
        gtk_style_context_add_class(gtk_widget_get_style_context(l), "heading");
    return l;
}

static GtkWidget *
panel_new(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *g, *sw;
    int row = 0;

    mw.panel_vals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          NULL);
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);
    mw.panel_title = gtk_label_new("No sector selected");
    gtk_label_set_xalign(GTK_LABEL(mw.panel_title), 0);
    gtk_label_set_line_wrap(GTK_LABEL(mw.panel_title), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(mw.panel_title),
                                "title");
    gtk_container_add(GTK_CONTAINER(box), mw.panel_title);

    /* our units in the sector, filled in by panel_show_sector */
    mw.panel_units = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(mw.panel_units), 0);
    gtk_label_set_line_wrap(GTK_LABEL(mw.panel_units), TRUE);
    gtk_label_set_selectable(GTK_LABEL(mw.panel_units), TRUE);
    gtk_widget_set_can_focus(mw.panel_units, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(mw.panel_units),
                                "units");
    gtk_container_add(GTK_CONTAINER(box), mw.panel_units);

    g = mw.panel_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(g), 10);
    gtk_grid_set_row_spacing(GTK_GRID(g), 1);
    for (guint i = 0; i < G_N_ELEMENTS(panel_general); i++, row++) {
        gtk_grid_attach(GTK_GRID(g), name_label(panel_general[i].label, FALSE),
                        0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(g), value_label(panel_general[i].key), 1, row,
                        2, 1);
    }
    gtk_grid_attach(GTK_GRID(g), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                    0, row++, 3, 1);
    gtk_grid_attach(GTK_GRID(g), name_label("", TRUE), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), name_label("Have", TRUE), 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), name_label("Thresh", TRUE), 2, row++, 1, 1);
    for (guint i = 0; i < G_N_ELEMENTS(panel_items); i++, row++) {
        gtk_grid_attach(GTK_GRID(g), name_label(panel_items[i][2], FALSE), 0,
                        row, 1, 1);
        gtk_grid_attach(GTK_GRID(g), value_label(panel_items[i][0]), 1, row, 1,
                        1);
        gtk_grid_attach(GTK_GRID(g), value_label(panel_items[i][1]), 2, row, 1,
                        1);
    }
    gtk_grid_attach(GTK_GRID(g), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                    0, row++, 3, 1);
    gtk_grid_attach(GTK_GRID(g), name_label("Resources", TRUE), 0, row++, 3, 1);
    for (guint i = 0; i < G_N_ELEMENTS(panel_res); i++, row++) {
        gtk_grid_attach(GTK_GRID(g), name_label(panel_res[i].label, FALSE), 0,
                        row, 1, 1);
        gtk_grid_attach(GTK_GRID(g), value_label(panel_res[i].key), 1, row, 2,
                        1);
    }
    gtk_container_add(GTK_CONTAINER(box), g);
    gtk_widget_set_sensitive(g, FALSE);

    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), box);
    gtk_widget_set_size_request(sw, 210, -1);
    return sw;
}

static void
panel_clear_values(void)
{
    GHashTableIter it;
    gpointer k, v;

    g_hash_table_iter_init(&it, mw.panel_vals);
    while (g_hash_table_iter_next(&it, &k, &v))
        gtk_label_set_text(GTK_LABEL(v), "");
}

static void
panel_set(const char *key, const char *val)
{
    GtkWidget *l = g_hash_table_lookup(mw.panel_vals, key);

    if (l)
        gtk_label_set_text(GTK_LABEL(l), val ? val : "");
}

/* What we have seen of a sector that isn't ours.  TRUE if anything. */
static gboolean
panel_intel_sector(int x, int y, GString *title)
{
    static const char *cols[] = {
        "eff", "effic", "civ", "civil", "mil", "milit", "shell", "shell",
        "gun", "gun", "petrol", "petrol", "food", "food", "bar", "bar",
        "road", "road", "rail", "rail", "defense", "dfense", NULL
    };
    sqlite3_stmt *st;
    gboolean have = FALSE;

    if (!db_table_exists("intel_sect")
        || sqlite3_prepare_v2(db_handle(), "SELECT * FROM intel_sect WHERE x=? "
                              "AND y=?", -1, &st, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_int(st, 1, x);
    sqlite3_bind_int(st, 2, y);
    if (sqlite3_step(st) == SQLITE_ROW) {
        int n = sqlite3_column_count(st);
        gint64 seen = 0;
        const char *source = "";

        have = TRUE;
        for (int i = 0; i < n; i++) {
            const char *col = sqlite3_column_name(st, i);
            gboolean null = sqlite3_column_type(st, i) == SQLITE_NULL;
            const char *v = (const char *)sqlite3_column_text(st, i);

            if (!strcmp(col, "owner") && !null) {
                int o = sqlite3_column_int(st, i);
                char *t = g_strdup_printf("%s (#%d)", world_country_name(o), o);

                panel_set("owner", t);
                g_free(t);
            } else if (!strcmp(col, "seen")) {
                seen = sqlite3_column_int64(st, i);
            } else if (!strcmp(col, "source")) {
                source = v ? v : "";
            } else if (!null) {
                for (int j = 0; cols[j]; j += 2) {
                    if (strcmp(col, cols[j]))
                        continue;
                    if (!strcmp(col, "eff") || !strcmp(col, "road")
                        || !strcmp(col, "rail") || !strcmp(col, "defense")) {
                        char *t = g_strdup_printf("%s%%", v);

                        panel_set(cols[j + 1], t);
                        g_free(t);
                    } else {
                        panel_set(cols[j + 1], v);
                    }
                }
            }
        }
        {
            char *age = intel_age(seen);

            g_string_append_printf(title, "\nseen %s%s%s", age,
                                   *source ? " by " : "", source);
            g_free(age);
        }
    }
    sqlite3_finalize(st);
    return have;
}

/* Other countries' units seen in a sector; appends lines to u */
static int
panel_intel_units(int x, int y, GString *u, int *shown)
{
    sqlite3_stmt *st;
    int total = 0;

    if (!db_table_exists("intel_unit")
        || sqlite3_prepare_v2(db_handle(), "SELECT cls, uid, owner, type, mil, "
                              "seen FROM intel_unit WHERE x=? AND y=? "
                              "ORDER BY cls, uid", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int(st, 1, x);
    sqlite3_bind_int(st, 2, y);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int cls = sqlite3_column_int(st, 0), own = sqlite3_column_int(st, 2);
        int type = sqlite3_column_int(st, 3);
        char *age;

        if (cls < 0 || cls >= UC_COUNT)
            continue;
        total++;
        if (*shown >= 30)
            continue;
        age = intel_age(sqlite3_column_int64(st, 5));
        g_string_append_printf(u, "%s%s: %s #%d  %s %s, %s",
                               u->len ? "\n" : "",
                               own >= 0 ? world_country_name(own) : "unknown",
                               world_unit_noun(cls, FALSE),
                               sqlite3_column_int(st, 1),
                               world_unit_mnem(cls, type),
                               world_unit_type_name(cls, type), age);
        if (sqlite3_column_type(st, 4) != SQLITE_NULL)
            g_string_append_printf(u, " (~%d mil)", sqlite3_column_int(st, 4));
        g_free(age);
        (*shown)++;
    }
    sqlite3_finalize(st);
    return total;
}

void
panel_show_sector(int x, int y)
{
    Sector *s = world_sector(x, y);
    sqlite3_stmt *st = NULL;
    GString *title = g_string_new(NULL);
    gboolean have = FALSE;

    panel_clear_values();
    if (s && s->des)
        g_string_printf(title, "%s  %d,%d", *world_des_name(s->des)
                        ? world_des_name(s->des) : "unknown", x, y);
    else
        g_string_printf(title, "%d,%d  (not seen)", x, y);

    if (db_is_open() && db_table_exists("sect")
        && sqlite3_prepare_v2(db_handle(), "SELECT * FROM sect WHERE xloc=? "
                              "AND yloc=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, x);
        sqlite3_bind_int(st, 2, y);
        if (sqlite3_step(st) == SQLITE_ROW) {
            int n = sqlite3_column_count(st);
            int dx = 0, dy = 0;

            have = TRUE;
            for (int i = 0; i < n; i++) {
                const char *col = sqlite3_column_name(st, i);
                const char *v = (const char *)sqlite3_column_text(st, i);

                if (!col)
                    continue;
                if (!strcmp(col, "owner")) {
                    int o = sqlite3_column_int(st, i);
                    char *t = g_strdup_printf("%s (#%d)",
                                              world_country_name(o), o);
                    panel_set(col, t);
                    g_free(t);
                } else if (!strcmp(col, "effic") || !strcmp(col, "work")
                           || !strcmp(col, "road") || !strcmp(col, "rail")
                           || !strcmp(col, "dfense")) {
                    char *t = g_strdup_printf("%s%%", v ? v : "0");
                    panel_set(col, t);
                    g_free(t);
                } else if (!strcmp(col, "xdist")) {
                    dx = sqlite3_column_int(st, i);
                } else if (!strcmp(col, "ydist")) {
                    dy = sqlite3_column_int(st, i);
                } else if (g_str_has_suffix(col, "_dist")) {
                    /* thresholds of 0 mean none */
                    panel_set(col, sqlite3_column_int(st, i) ? v : "");
                } else {
                    panel_set(col, v);
                }
            }
            if (dx != x || dy != y) {
                char *t = g_strdup_printf("%d,%d", dx, dy);
                panel_set("dist", t);
                g_free(t);
            }
            if (s && s->newdes && *world_des_name(s->newdes))
                g_string_append_printf(title, "\nbecoming %s",
                                       world_des_name(s->newdes));
        }
        sqlite3_finalize(st);
    }
    if (!have)
        have = panel_intel_sector(x, y, title);
    gtk_label_set_text(GTK_LABEL(mw.panel_title), title->str);

    /* our units in this sector */
    {
        GString *u = g_string_new(NULL);
        int shown = 0, total = 0;

        for (int c = 0; c < UC_COUNT; c++) {
            GPtrArray *a = world_units(c);

            for (guint i = 0; i < a->len; i++) {
                Unit *un = g_ptr_array_index(a, i);
                char *d;

                if (un->x != x || un->y != y)
                    continue;
                total++;
                if (shown >= 30)
                    continue;
                d = world_unit_describe(un);
                g_string_append_printf(u, "%s%s", u->len ? "\n" : "", d);
                g_free(d);
                shown++;
            }
        }
        total += panel_intel_units(x, y, u, &shown);
        if (total > shown)
            g_string_append_printf(u, "\n... and %d more", total - shown);
        if (total) {
            char *head = g_strdup_printf("Units here (%d):\n", total);

            g_string_prepend(u, head);
            g_free(head);
        }
        gtk_label_set_text(GTK_LABEL(mw.panel_units), u->str);
        gtk_widget_set_visible(mw.panel_units, total > 0);
        g_string_free(u, TRUE);
    }
    gtk_widget_set_sensitive(mw.panel_grid, have);
    g_string_free(title, TRUE);
}

/* ------------------------------------------------------- refreshing */

static void
redisplay(void)
{
    int x, y;

    world_load();
    mapview_redraw();
    if (mapview_selected(&x, &y))
        panel_show_sector(x, y);
    units_window_refresh();
}

static void
refresh_finished(gpointer data)
{
    const char *what = data;

    mw.refreshing = FALSE;
    redisplay();
    if (conn_state() == CONN_PLAYING && what) {
        GString *t = g_string_new(NULL);

        g_string_printf(t, "%s updated: %d sectors", what,
                        world_sector_count());
        for (int c = 0; c < UC_COUNT; c++) {
            int n = world_units(c)->len;

            if (n)
                g_string_append_printf(t, ", %d %s", n,
                                       world_unit_noun(c, n != 1));
        }
        g_string_append_c(t, '.');
        out_text(t->str, "notice");
        g_string_free(t, TRUE);
    }
}

static void
fetch_units(void)
{
    for (int c = 0; c < UC_COUNT; c++)
        db_fetch_table(world_unit_table(c), NULL, NULL);
}

void
refresh_all(void)
{
    if (conn_state() != CONN_PLAYING || mw.refreshing)
        return;
    mw.refreshing = TRUE;
    out_text("Updating the database...", "notice");
    db_fetch_table("version", NULL, NULL);
    db_fetch_table("sect-chr", NULL, NULL);
    db_fetch_table("ship-chr", NULL, NULL);
    db_fetch_table("land-chr", NULL, NULL);
    db_fetch_table("plane-chr", NULL, NULL);
    db_fetch_table("nuke-chr", NULL, NULL);
    db_fetch_table("nat", NULL, NULL);
    db_fetch_table("country", NULL, NULL);
    db_fetch_table("relat", NULL, NULL);
    db_fetch_table("sect", NULL, NULL);
    fetch_units();
    db_fetch_bmap(refresh_finished, "Database");
}

void
refresh_units(void)
{
    if (conn_state() != CONN_PLAYING)
        return;
    for (int c = 0; c < UC_COUNT; c++)
        db_fetch_table(world_unit_table(c), c == UC_COUNT - 1
                       ? refresh_finished : NULL, "Units");
}

/* --- refreshing after commands that change sectors or units --- */

static const char *changing_cmds[] = {
    /* sectors */
    "move", "designate", "distribute", "threshold", "deliver", "explore",
    "build", "demobilize", "enlist", "convert", "improve", "collect",
    "wipe", "transport", "grind", "harvest", "start", "stop",
    /* units */
    "navigate", "march", "fly", "load", "unload", "lload", "lunload",
    "tend", "ltend", "scrap", "scuttle", "upgrade", "fleetadd", "army",
    "wing", "sail", "unsail", "follow", "retreat", "lretreat", "mission",
    "arm", "disarm", "launch", "drop", "paradrop", "bomb", "recon",
    "sweep", "name", "fuel", "morale", "supply", "harden", "fortify",
    "assault", "attack", "board", "lmine", "mine", "torpedo", "fire",
    "transport", "order", "qorder", "sorder",
    /* these update the bmap on the server */
    "lookout", "llook", "radar", "lradar", "sonar", "coastwatch",
    "skywatch", "spy",
    /* diplomacy changes how other countries' units are shown */
    "declare",
    /* deity */
    "edit", "give", "setsector", "setresource", "swapsector",
    NULL
};

static guint changed_timer;

static gboolean
changed_refresh(gpointer d)
{
    if (conn_state() != CONN_PLAYING) {
        changed_timer = 0;
        return G_SOURCE_REMOVE;
    }
    if (conn_busy() || conn_wants_input() || mw.refreshing)
        return G_SOURCE_CONTINUE;       /* wait for a quiet moment */
    changed_timer = 0;
    db_fetch_table("relat", NULL, NULL);
    db_fetch_table("sect", NULL, NULL);
    fetch_units();
    db_fetch_bmap(refresh_finished, NULL);
    return G_SOURCE_REMOVE;
}

void
refresh_after_command(const char *cmd)
{
    char word[32];
    size_t len;

    if (sscanf(cmd, " %31[a-zA-Z]", word) != 1)
        return;
    len = strlen(word);
    for (char *p = word; *p; p++)
        *p = g_ascii_tolower(*p);
    for (int i = 0; changing_cmds[i]; i++) {
        /* Empire accepts unique abbreviations */
        if (len >= 2 && !strncmp(changing_cmds[i], word, len)) {
            if (changed_timer)
                g_source_remove(changed_timer);
            changed_timer = g_timeout_add(1200, changed_refresh, NULL);
            return;
        }
    }
}

void
refresh_sectors(void)
{
    if (conn_state() != CONN_PLAYING)
        return;
    db_fetch_table("sect", refresh_finished, "Sectors");
}

void
refresh_bmap(void)
{
    if (conn_state() != CONN_PLAYING)
        return;
    db_fetch_bmap(refresh_finished, "Bmap");
}

void
mainwin_data_changed(void)
{
    redisplay();
}

/* ------------------------------------------------- game lifecycle */

static void
set_title(gboolean offline)
{
    char *t;

    if (!mw.game)
        t = g_strdup(GTKACE_NAME);
    else
        t = g_strdup_printf("%s - %s%s", GTKACE_NAME, mw.game->name,
                            offline ? " (offline)" : "");
    gtk_window_set_title(GTK_WINDOW(mw.win), t);
    g_free(t);
}

void
mainwin_set_game(const Game *g, gboolean connect)
{
    char *path;

    conn_close(NULL);
    game_free(mw.game);
    mw.game = game_copy(g);
    path = game_db_path(g);
    if (!db_open(path)) {
        char *m = g_strdup_printf("Could not open the database %s", path);
        mainwin_error("Database error", m);
        g_free(m);
    }
    g_free(path);
    db_set_setting("country", g->country);
    world_load();
    units_window_refresh();
    mapview_center(0, 0);
    mapview_select(0, 0);
    set_title(!connect);
    games_touch(g->name);
    if (connect)
        conn_start(g);
    else
        status_set_conn(CONN_DOWN, "Working offline");
}

void
mainwin_logged_in(void)
{
    set_title(FALSE);
    db_new_session();
    gtk_widget_grab_focus(mw.entry);
    if (mw.game->autoupdate || world_x <= 0)
        refresh_all();
}

void
mainwin_disconnected(void)
{
    mw.refreshing = FALSE;
    set_title(TRUE);
}

/* ------------------------------------------------------- menu items */

static void
open_game(void)
{
    gboolean connect = FALSE;
    Game *g = game_choose(GTK_WINDOW(mw.win), &connect);

    if (g) {
        mainwin_set_game(g, connect);
        game_free(g);
    }
}

static void
act_open(GtkWidget *w, gpointer d)
{
    open_game();
}

static void
act_connect(GtkWidget *w, gpointer d)
{
    if (mw.game && conn_state() == CONN_DOWN) {
        set_title(FALSE);
        conn_start(mw.game);
    }
}

static void
act_disconnect(GtkWidget *w, gpointer d)
{
    if (conn_state() == CONN_PLAYING)
        conn_command("bye", CMD_SHOW, NULL, NULL, NULL);
    else
        conn_close("Disconnected.");
}

static void
act_quit(GtkWidget *w, gpointer d)
{
    gtk_widget_destroy(mw.win);
}

static void
act_cmd(GtkWidget *w, gpointer cmd)
{
    conn_command(cmd, CMD_SHOW | CMD_USER, NULL, NULL, NULL);
}

static void
act_zoom_in(GtkWidget *w, gpointer d)
{
    mapview_zoom(1.25);
}

static void
act_zoom_out(GtkWidget *w, gpointer d)
{
    mapview_zoom(0.8);
}

static void
act_center(GtkWidget *w, gpointer d)
{
    int x, y;

    if (mapview_selected(&x, &y))
        mapview_center(x, y);
}

static void
act_capital(GtkWidget *w, gpointer d)
{
    mapview_center(0, 0);
    mapview_select(0, 0);
}

static void
act_goto(GtkWidget *w, gpointer d)
{
    GtkWidget *dlg, *entry, *box;
    int x, y;

    dlg = gtk_dialog_new_with_buttons("Go to Sector", GTK_WINDOW(mw.win),
                                      GTK_DIALOG_MODAL, "_Cancel",
                                      GTK_RESPONSE_CANCEL, "_Go",
                                      GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_set_spacing(GTK_BOX(box), 6);
    gtk_container_add(GTK_CONTAINER(box), gtk_label_new("Sector (x,y):"));
    entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_container_add(GTK_CONTAINER(box), entry);
    gtk_widget_show_all(dlg);
    while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        if (sscanf(gtk_entry_get_text(GTK_ENTRY(entry)), "%d , %d", &x, &y)
            == 2 && !((x + y) & 1)) {
            mapview_center(x, y);
            mapview_select(x, y);
            break;
        }
        gtk_entry_set_text(GTK_ENTRY(entry), "");
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
                                       "e.g. 2,0 (x + y must be even)");
    }
    gtk_widget_destroy(dlg);
}

static void
act_refresh_all(GtkWidget *w, gpointer d)
{
    refresh_all();
}

static void
act_refresh_sect(GtkWidget *w, gpointer d)
{
    refresh_sectors();
}

static void
act_refresh_bmap(GtkWidget *w, gpointer d)
{
    refresh_bmap();
}

static void
act_refresh_units(GtkWidget *w, gpointer d)
{
    refresh_units();
}

static void
act_units(GtkWidget *w, gpointer c)
{
    units_window_show(GPOINTER_TO_INT(c));
}

static void
act_show_units(GtkCheckMenuItem *ck, gpointer c)
{
    show_units[GPOINTER_TO_INT(c)] = gtk_check_menu_item_get_active(ck);
    mapview_redraw();
}

static void
act_refresh_nat(GtkWidget *w, gpointer d)
{
    db_fetch_table("nat", refresh_finished, "Country list");
}

static void
act_clear(GtkWidget *w, gpointer d)
{
    gtk_text_buffer_set_text(mw.buf, "", -1);
}

/* Commands menu: dialogs start from the selected sector */
static void
act_dlg(GtkWidget *w, gpointer which)
{
    int x = 0, y = 0;
    gboolean sel = mapview_selected(&x, &y);

    switch (GPOINTER_TO_INT(which)) {
    case 1: dlg_move(x, y, sel); break;
    case 2: dlg_nav(UC_SHIP, NULL); break;
    case 3: dlg_nav(UC_LAND, NULL); break;
    case 4: dlg_build(x, y, sel); break;
    case 5: dlg_load(UC_SHIP, NULL); break;
    case 6: dlg_load(UC_LAND, NULL); break;
    }
}

static void
act_clear_intel(GtkWidget *w, gpointer d)
{
    GtkWidget *m;

    if (!db_is_open())
        return;
    m = gtk_message_dialog_new(GTK_WINDOW(mw.win), GTK_DIALOG_MODAL,
                               GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                               "Forget everything seen of other countries?");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(m), "This removes the sectors and units recorded "
        "from lookout, coastwatch, radar, sonar and spy reports in this "
        "game.  Your own data is kept.");
    if (gtk_dialog_run(GTK_DIALOG(m)) == GTK_RESPONSE_YES)
        intel_clear();
    gtk_widget_destroy(m);
}

static void
act_about(GtkWidget *w, gpointer d)
{
    const char *authors[] = {
        "GtkACE port to C and GTK 3 (2026)",
        "",
        "WinACE, from which GtkACE is derived:",
        "Original design and programming: Jim Simons (Escher)",
        "Collaboration and testing: Ken McDonald (also Escher)",
        "Updates and maintenance since v2.2.0: Daniel Kiracofe (Bwian)",
        "Updates and maintenance since v2.3.0: Ron Koenderink (RimFire)",
        "Thanks to Jon Butler for the original WS Empire code",
        NULL
    };

    gtk_show_about_dialog(GTK_WINDOW(mw.win),
                          "program-name", GTKACE_NAME,
                          "version", GTKACE_VERSION,
                          "comments", "A graphical client for Wolfpack Empire.\n"
                          "Based on WinACE 2.5.72, the Windows Alternate "
                          "Client for Empire.\nGtkACE is a modified version "
                          "and is not WinACE.",
                          "copyright", "WinACE \302\251 1997-2001 James A. "
                          "Simons, \302\251 2002 Daniel R. Kiracofe,\n"
                          "and its later maintainers",
                          "license", "GtkACE is distributed under the Artistic "
                          "License, like WinACE.\nSee the LICENSE file for "
                          "the full text.",
                          "wrap-license", TRUE,
                          "authors", authors,
                          "logo-icon-name", "applications-games",
                          NULL);
}

/* ---------------------------------------------------- menu building */

static GtkAccelGroup *accel;

static GtkWidget *
add_menu(GtkWidget *bar, const char *label)
{
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *menu = gtk_menu_new();

    gtk_menu_set_accel_group(GTK_MENU(menu), accel);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), item);
    return menu;
}

static GtkWidget *
add_item(GtkWidget *menu, const char *label, GCallback cb, gpointer data,
         gboolean needs_conn, const char *accel_str)
{
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);

    if (cb)
        g_signal_connect(item, "activate", cb, data);
    if (accel_str) {
        guint key;
        GdkModifierType mods;

        gtk_accelerator_parse(accel_str, &key, &mods);
        gtk_widget_add_accelerator(item, "activate", accel, key, mods,
                                   GTK_ACCEL_VISIBLE);
    }
    if (needs_conn) {
        mw.needs_conn = g_list_prepend(mw.needs_conn, item);
        gtk_widget_set_sensitive(item, FALSE);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static void
add_sep(GtkWidget *menu)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static void
add_cmd(GtkWidget *menu, const char *label, const char *cmd)
{
    add_item(menu, label, G_CALLBACK(act_cmd), (gpointer)cmd, TRUE, NULL);
}

static GtkWidget *
build_menus(void)
{
    GtkWidget *bar = gtk_menu_bar_new(), *m, *sub, *it;

    m = add_menu(bar, "_File");
    add_item(m, "_Open Game...", G_CALLBACK(act_open), NULL, FALSE,
             "<Control>o");
    mw.connect_item = add_item(m, "_Connect", G_CALLBACK(act_connect), NULL,
                               FALSE, NULL);
    gtk_widget_set_sensitive(mw.connect_item, FALSE);
    add_item(m, "_Disconnect", G_CALLBACK(act_disconnect), NULL, TRUE, NULL);
    add_sep(m);
    add_item(m, "C_lear Output", G_CALLBACK(act_clear), NULL, FALSE, NULL);
    add_item(m, "Clear _Intelligence...", G_CALLBACK(act_clear_intel), NULL,
             FALSE, NULL);
    add_sep(m);
    add_item(m, "_Quit", G_CALLBACK(act_quit), NULL, FALSE, "<Control>q");

    m = add_menu(bar, "_View");
    add_item(m, "Zoom _In", G_CALLBACK(act_zoom_in), NULL, FALSE,
             "<Control>plus");
    add_item(m, "Zoom _Out", G_CALLBACK(act_zoom_out), NULL, FALSE,
             "<Control>minus");
    add_sep(m);
    add_item(m, "_Center on Selection", G_CALLBACK(act_center), NULL, FALSE,
             NULL);
    add_item(m, "Go to C_apital Origin", G_CALLBACK(act_capital), NULL, FALSE,
             "Home");
    add_item(m, "_Go to Sector...", G_CALLBACK(act_goto), NULL, FALSE,
             "<Control>g");
    add_sep(m);
    for (int c = 0; c < UC_COUNT; c++) {
        static const char *labels[UC_COUNT] = {
            "Show _Ships", "Show _Land Units", "Show _Planes", "Show _Nukes"
        };
        GtkWidget *ck = gtk_check_menu_item_new_with_mnemonic(labels[c]);

        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(ck), show_units[c]);
        g_signal_connect(ck, "toggled", G_CALLBACK(act_show_units),
                         GINT_TO_POINTER(c));
        gtk_menu_shell_append(GTK_MENU_SHELL(m), ck);
    }

    m = add_menu(bar, "_Refresh");
    add_item(m, "Update _All", G_CALLBACK(act_refresh_all), NULL, TRUE, "F5");
    add_sep(m);
    add_item(m, "Update Se_ctors", G_CALLBACK(act_refresh_sect), NULL, TRUE,
             NULL);
    add_item(m, "Update _Bmap", G_CALLBACK(act_refresh_bmap), NULL, TRUE,
             NULL);
    add_item(m, "Update _Units", G_CALLBACK(act_refresh_units), NULL, TRUE,
             NULL);
    add_item(m, "Update _Player List", G_CALLBACK(act_refresh_nat), NULL, TRUE,
             NULL);

    m = add_menu(bar, "_Units");
    add_item(m, "_Ships", G_CALLBACK(act_units), GINT_TO_POINTER(UC_SHIP),
             FALSE, "<Control>1");
    add_item(m, "_Land Units", G_CALLBACK(act_units), GINT_TO_POINTER(UC_LAND),
             FALSE, "<Control>2");
    add_item(m, "_Planes", G_CALLBACK(act_units), GINT_TO_POINTER(UC_PLANE),
             FALSE, "<Control>3");
    add_item(m, "_Nukes", G_CALLBACK(act_units), GINT_TO_POINTER(UC_NUKE),
             FALSE, "<Control>4");
    add_item(m, "_Foreign Units Seen", G_CALLBACK(act_units),
             GINT_TO_POINTER(UC_COUNT), FALSE, "<Control>5");
    add_sep(m);
    add_cmd(m, "Ship _Cargo", "cargo *");
    add_cmd(m, "Land Unit Car_go", "lcargo *");
    add_cmd(m, "Ship S_tats", "sstat *");
    add_cmd(m, "Land Unit St_ats", "lstat *");
    add_cmd(m, "Plane Stat_s", "pstat *");
    add_sep(m);
    add_cmd(m, "Show Ship _Build Info", "show ship build");
    add_cmd(m, "Show Land Unit Build _Info", "show land build");
    add_cmd(m, "Show Plane Build In_fo", "show plane build");

    m = add_menu(bar, "_Commands");
    add_item(m, "_Move...", G_CALLBACK(act_dlg), GINT_TO_POINTER(1), FALSE,
             "<Control>m");
    add_item(m, "_Navigate Ships...", G_CALLBACK(act_dlg), GINT_TO_POINTER(2),
             FALSE, "<Control>n");
    add_item(m, "M_arch Land Units...", G_CALLBACK(act_dlg),
             GINT_TO_POINTER(3), FALSE, NULL);
    add_item(m, "_Build...", G_CALLBACK(act_dlg), GINT_TO_POINTER(4), FALSE,
             "<Control>b");
    add_sep(m);
    add_item(m, "_Load or Unload Ships...", G_CALLBACK(act_dlg),
             GINT_TO_POINTER(5), FALSE, "<Control>l");
    add_item(m, "Load or Unload L_and Units...", G_CALLBACK(act_dlg),
             GINT_TO_POINTER(6), FALSE, NULL);

    m = add_menu(bar, "Re_ports");
    add_cmd(m, "_Budget", "budget");
    add_cmd(m, "_Census", "census *");
    add_cmd(m, "Coast_watch", "coastwatch *");
    add_cmd(m, "Co_mmodity", "commodity *");
    it = add_item(m, "_Intelligence", NULL, NULL, FALSE, NULL);
    sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(it), sub);
    add_cmd(sub, "_Lookout (all ships)", "lookout *");
    add_cmd(sub, "L_and Unit Lookout", "llook *");
    add_cmd(sub, "_Radar (all ships)", "radar *");
    add_cmd(sub, "Land Unit Ra_dar", "lradar *");
    add_cmd(sub, "_Sonar (all ships)", "sonar *");
    add_cmd(sub, "_Coastwatch", "coastwatch *");
    add_cmd(sub, "S_kywatch", "skywatch *");
    add_cmd(sub, "S_py (all sectors)", "spy *");
    add_cmd(m, "_Level", "level *");
    it = add_item(m, "M_aps", NULL, NULL, FALSE, NULL);
    sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(it), sub);
    add_cmd(sub, "_Map", "map *");
    add_cmd(sub, "_Bmap", "bmap *");
    add_cmd(sub, "_Nmap", "nmap *");
    add_cmd(sub, "_Smap", "smap *");
    add_cmd(sub, "_Lmap", "lmap *");
    add_cmd(sub, "_Pmap", "pmap *");
    add_cmd(m, "_Nation", "nation");
    add_cmd(m, "Ne_wspaper", "news");
    add_cmd(m, "Pla_yers", "players");
    add_cmd(m, "_Power (new)", "power new");
    add_cmd(m, "P_roduction", "production *");
    add_cmd(m, "_Report (tech/edu/hap)", "report *");
    add_cmd(m, "Reso_urce", "resource *");
    add_cmd(m, "_Show", "show sect build");
    add_cmd(m, "Star_vation", "starvation *");
    add_cmd(m, "_Update Times", "update");
    add_cmd(m, "_Version", "version");
    add_cmd(m, "W_ire", "wire");

    m = add_menu(bar, "_Diplomacy");
    add_cmd(m, "_Read Telegrams", "read");
    add_cmd(m, "R_elations", "relations");
    add_cmd(m, "_Countries", "country *");

    m = add_menu(bar, "_Help");
    add_cmd(m, "_Command List", "list");
    add_cmd(m, "_Info Index", "info");
    add_sep(m);
    add_item(m, "_About", G_CALLBACK(act_about), NULL, FALSE, NULL);
    return bar;
}

static void
tool_button(GtkWidget *tb, const char *label, const char *tip, GCallback cb,
            gpointer data)
{
    GtkToolItem *b = gtk_tool_button_new(NULL, label);

    gtk_tool_item_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, data);
    gtk_toolbar_insert(GTK_TOOLBAR(tb), b, -1);
    mw.needs_conn = g_list_prepend(mw.needs_conn, b);
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
}

static GtkWidget *
build_toolbar(void)
{
    GtkWidget *tb = gtk_toolbar_new();

    gtk_toolbar_set_style(GTK_TOOLBAR(tb), GTK_TOOLBAR_TEXT);
    tool_button(tb, "Update", "Update all sector and map information now",
                G_CALLBACK(act_refresh_all), NULL);
    gtk_toolbar_insert(GTK_TOOLBAR(tb), gtk_separator_tool_item_new(), -1);
    tool_button(tb, "Budget", "Run the budget report", G_CALLBACK(act_cmd),
                "budget");
    tool_button(tb, "Coastwatch", "Run the coastwatch report",
                G_CALLBACK(act_cmd), "coastwatch *");
    tool_button(tb, "Nation", "Run the nation report", G_CALLBACK(act_cmd),
                "nation");
    tool_button(tb, "News", "Get today's paper", G_CALLBACK(act_cmd), "news");
    tool_button(tb, "Power", "Get a new power report", G_CALLBACK(act_cmd),
                "power new");
    tool_button(tb, "Report", "Get the tech report", G_CALLBACK(act_cmd),
                "report *");
    tool_button(tb, "Starvation", "Check starvation", G_CALLBACK(act_cmd),
                "starvation *");
    tool_button(tb, "Wire", "Read today's wires", G_CALLBACK(act_cmd), "wire");
    gtk_toolbar_insert(GTK_TOOLBAR(tb), gtk_separator_tool_item_new(), -1);
    tool_button(tb, "Read", "Read telegrams", G_CALLBACK(act_cmd), "read");
    tool_button(tb, "Relations", "Check relations", G_CALLBACK(act_cmd),
                "relations");
    return tb;
}

/* ------------------------------------------------------ command line */

static void
history_add(const char *cmd)
{
    if (!*cmd)
        return;
    if (mw.history->len
        && !strcmp(g_ptr_array_index(mw.history, mw.history->len - 1), cmd))
        return;
    g_ptr_array_add(mw.history, g_strdup(cmd));
    if (mw.history->len > HISTORY_MAX)
        g_ptr_array_remove_index(mw.history, 0);
}

static void
entry_activate(GtkEntry *e, gpointer d)
{
    char *text = g_strdup(gtk_entry_get_text(e));

    gtk_entry_set_text(e, "");
    mw.hist_pos = -1;
    if (conn_wants_input()) {
        conn_answer(text);
    } else {
        char *t = g_strstrip(g_strdup(text));

        if (*t) {
            history_add(t);
            conn_command(t, CMD_SHOW | CMD_USER, NULL, NULL, NULL);
        }
        g_free(t);
    }
    g_free(text);
}

static gboolean
entry_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    int n = mw.history->len;

    if (ev->keyval == GDK_KEY_Escape
        || (ev->keyval == GDK_KEY_c && (ev->state & GDK_CONTROL_MASK)
            && !gtk_editable_get_selection_bounds(GTK_EDITABLE(w), NULL,
                                                  NULL))) {
        if (conn_wants_input()) {
            conn_abort();
            return TRUE;
        }
        return FALSE;
    }
    if (ev->keyval != GDK_KEY_Up && ev->keyval != GDK_KEY_Down)
        return FALSE;
    if (!n)
        return TRUE;
    if (mw.hist_pos < 0) {
        if (ev->keyval == GDK_KEY_Down)
            return TRUE;
        g_free(mw.hist_draft);
        mw.hist_draft = g_strdup(gtk_entry_get_text(GTK_ENTRY(w)));
        mw.hist_pos = n;
    }
    mw.hist_pos += ev->keyval == GDK_KEY_Up ? -1 : 1;
    if (mw.hist_pos < 0)
        mw.hist_pos = 0;
    if (mw.hist_pos >= n) {
        mw.hist_pos = -1;
        gtk_entry_set_text(GTK_ENTRY(w), mw.hist_draft ? mw.hist_draft : "");
    } else {
        gtk_entry_set_text(GTK_ENTRY(w), g_ptr_array_index(mw.history,
                                                           mw.hist_pos));
    }
    gtk_editable_set_position(GTK_EDITABLE(w), -1);
    return TRUE;
}

/* -------------------------------------------------------- the window */

static const char css[] =
    ".output { font-family: Monospace; }\n"
    "label.question { color: #b00000; font-weight: bold; }\n"
    "label.title { font-weight: bold; }\n"
    "label.heading { font-weight: bold; }\n"
    "label.value { font-family: Monospace; }\n"
    "label.units { color: #1a4fb5; padding-bottom: 4px; }\n"
    "label.preview { font-family: Monospace; color: #1a4fb5; }\n"
    ".statusfield { padding: 1px 6px; }\n";

static GtkWidget *
status_field(GtkWidget *box, gboolean expand)
{
    GtkWidget *l = gtk_label_new("");

    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(l),
                                "statusfield");
    gtk_box_pack_start(GTK_BOX(box), l, expand, expand, 0);
    if (!expand)
        gtk_box_pack_start(GTK_BOX(box),
                           gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                           FALSE, FALSE, 0);
    return l;
}

static void
destroyed(GtkWidget *w, gpointer d)
{
    conn_close(NULL);
    db_close();
    mw.win = NULL;
    mw.buf = NULL;
}

GtkWidget *
mainwin_new(GtkApplication *app)
{
    GtkWidget *vbox, *hpaned, *vpaned, *sw, *outbox, *ebox, *status;
    GtkCssProvider *prov;
    GtkTextIter it;

    mw.history = g_ptr_array_new_with_free_func(g_free);
    mw.hist_pos = -1;
    mw.win = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(mw.win), 1000, 760);
    gtk_window_set_icon_name(GTK_WINDOW(mw.win), "gtkace");
    g_signal_connect(mw.win, "destroy", G_CALLBACK(destroyed), NULL);
    accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(mw.win), accel);

    prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(mw.win), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), build_menus(), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_toolbar(), FALSE, FALSE, 0);

    /* map | sector panel, above the output */
    vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(hpaned), mapview_new(), TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(hpaned), panel_new(), FALSE, FALSE);
    gtk_paned_pack1(GTK_PANED(vpaned), hpaned, TRUE, FALSE);

    outbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    mw.output = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(mw.output), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(mw.output), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(mw.output), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(mw.output), 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(mw.output),
                                "output");
    mw.buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(mw.output));
    gtk_text_buffer_create_tag(mw.buf, "command", "foreground", "#1a4fb5",
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(mw.buf, "answer", "foreground", "#1a7f2b",
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(mw.buf, "question", "foreground", "#b00000",
                               NULL);
    gtk_text_buffer_create_tag(mw.buf, "notice", "foreground", "#666666",
                               "style", PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag(mw.buf, "server", "foreground", "#555555",
                               NULL);
    gtk_text_buffer_create_tag(mw.buf, "error", "foreground", "#b00000", NULL);
    gtk_text_buffer_create_tag(mw.buf, "flash", "foreground", "#8a1a9b",
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(mw.buf, "highlight", "weight",
                               PANGO_WEIGHT_BOLD, "background", "#fff2a8",
                               NULL);
    gtk_text_buffer_get_end_iter(mw.buf, &it);
    mw.end = gtk_text_buffer_create_mark(mw.buf, "end", &it, FALSE);
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), mw.output);
    gtk_box_pack_start(GTK_BOX(outbox), sw, TRUE, TRUE, 0);

    ebox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(ebox), 2);
    mw.entry_label = gtk_label_new("Command:");
    gtk_label_set_ellipsize(GTK_LABEL(mw.entry_label), PANGO_ELLIPSIZE_START);
    gtk_label_set_max_width_chars(GTK_LABEL(mw.entry_label), 60);
    mw.entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(mw.entry),
                                   "Type an Empire command and press Enter");
    g_signal_connect(mw.entry, "activate", G_CALLBACK(entry_activate), NULL);
    g_signal_connect(mw.entry, "key-press-event", G_CALLBACK(entry_key), NULL);
    gtk_box_pack_start(GTK_BOX(ebox), mw.entry_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ebox), mw.entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outbox), ebox, FALSE, FALSE, 0);
    gtk_paned_pack2(GTK_PANED(vpaned), outbox, FALSE, FALSE);
    gtk_paned_set_position(GTK_PANED(vpaned), 470);
    gtk_box_pack_start(GTK_BOX(vbox), vpaned, TRUE, TRUE, 0);

    status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    mw.st_conn = status_field(status, FALSE);
    mw.st_time = status_field(status, FALSE);
    mw.st_mail = status_field(status, FALSE);
    mw.st_busy = status_field(status, FALSE);
    mw.st_hover = status_field(status, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(
                           GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status, FALSE, FALSE, 0);
    status_set_conn(CONN_DOWN, NULL);

    gtk_widget_show_all(mw.win);
    gtk_widget_grab_focus(mw.entry);
    return mw.win;
}
