/*
 * dialogs.c - command dialogs (WinACE frmPromptMove, frmPromptNav,
 * frmPromptBuild, frmPromptLoad).
 *
 * Each dialog builds an ordinary Empire command and shows it before it
 * is sent.  While a dialog is the active window, clicking a sector on
 * the map fills in its sector fields.  The Move dialog asks the server
 * for the best path and its mobility cost ("bestpath" and "test"), so
 * the figures are exactly what the move will cost.
 */
#include <string.h>
#include <stdlib.h>
#include <sqlite3.h>
#include "gtkace.h"

/* ---------------------------------------------------------- helpers */

typedef struct {
    char letter;
    const char *name;           /* as the server spells it */
    const char *label;
    const char *col;            /* xdump column */
} Item;

static const Item items[] = {
    { 'c', "civil", "civilians", "civil" }, { 'm', "milit", "military", "milit" },
    { 'u', "uw", "uncompensated workers", "uw" }, { 'f', "food", "food", "food" },
    { 's', "shell", "shells", "shell" }, { 'g', "gun", "guns", "gun" },
    { 'p', "petrol", "petrol", "petrol" }, { 'i', "iron", "iron ore", "iron" },
    { 'd', "dust", "gold dust", "dust" }, { 'b', "bar", "gold bars", "bar" },
    { 'o', "oil", "oil", "oil" }, { 'l', "lcm", "light construction materials",
                                    "lcm" },
    { 'h', "hcm", "heavy construction materials", "hcm" },
    { 'r', "rad", "radioactive materials", "rad" },
};

static gboolean
parse_sector(const char *s, int *x, int *y)
{
    char *end;
    long a, b;

    while (*s == ' ')
        s++;
    a = strtol(s, &end, 10);
    if (end == s || *end != ',')
        return FALSE;
    s = end + 1;
    b = strtol(s, &end, 10);
    if (end == s)
        return FALSE;
    while (*end == ' ')
        end++;
    if (*end)
        return FALSE;
    *x = a;
    *y = b;
    return world_x <= 0 || !((world_wrap_x(a) + world_wrap_y(b)) & 1);
}

/* A route of directions, as typed: letters from yugjbnh only */
static gboolean
is_route(const char *s)
{
    if (!*s)
        return FALSE;
    for (; *s; s++)
        if (!strchr("yugjbnhYUGJBNH", *s))
            return FALSE;
    return TRUE;
}

/* A route must end with h, or the server asks for more */
static char *
route_with_stop(const char *s)
{
    size_t n = strlen(s);

    return n && g_ascii_tolower(s[n - 1]) == 'h' ? g_strdup(s)
        : g_strconcat(s, "h", NULL);
}

static int
query_int(const char *sql, int x, int y, int dflt)
{
    sqlite3_stmt *st;
    int v = dflt;

    if (!db_is_open()
        || sqlite3_prepare_v2(db_handle(), sql, -1, &st, NULL) != SQLITE_OK)
        return dflt;
    sqlite3_bind_int(st, 1, x);
    sqlite3_bind_int(st, 2, y);
    if (sqlite3_step(st) == SQLITE_ROW
        && sqlite3_column_type(st, 0) != SQLITE_NULL)
        v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* Value of a column of one of our sectors, or dflt */
static int
sect_value(int x, int y, const char *col, int dflt)
{
    char *sql;
    int v;

    if (!db_table_exists("sect"))
        return dflt;
    sql = g_strdup_printf("SELECT \"%s\" FROM sect WHERE xloc=? AND yloc=?",
                          col);
    v = query_int(sql, x, y, dflt);
    g_free(sql);
    return v;
}

static gboolean
our_sector(int x, int y)
{
    return sect_value(x, y, "owner", -1) >= 0;
}

/* Our units of a class in a sector, as "3/4/7" (newly allocated) */
static char *
units_at(UnitClass c, int x, int y)
{
    GPtrArray *a = world_units(c);
    GString *s = g_string_new(NULL);

    for (guint i = 0; i < a->len; i++) {
        Unit *u = g_ptr_array_index(a, i);

        if (u->x == x && u->y == y && u->carrier < 0)
            g_string_append_printf(s, "%s%d", s->len ? "/" : "", u->uid);
    }
    return g_string_free(s, FALSE);
}

static Unit *
find_unit(UnitClass c, const char *spec)
{
    char *end;
    long uid;
    GPtrArray *a = world_units(c);

    while (*spec == ' ')
        spec++;
    uid = strtol(spec, &end, 10);
    if (end == spec || *end)
        return NULL;
    for (guint i = 0; i < a->len; i++) {
        Unit *u = g_ptr_array_index(a, i);

        if (u->uid == uid)
            return u;
    }
    return NULL;
}

/* A spin button's number as typed: its value is only updated when
 * focus leaves it, which is too late for a live preview */
static int
spin_value(GtkWidget *w)
{
    const char *t = gtk_entry_get_text(GTK_ENTRY(w));
    char *end;
    long v = strtol(t, &end, 10);

    return end == t ? 0 : (int)v;
}

static const char *
entry_text(GtkWidget *e)
{
    return gtk_entry_get_text(GTK_ENTRY(e));
}

static char *
entry_trimmed(GtkWidget *e)
{
    return g_strstrip(g_strdup(entry_text(e)));
}

/* Common dialog frame: a window with a grid, an info area, the
 * command preview and buttons. */
typedef struct {
    GtkWidget *win, *grid, *info, *preview, *ok;
    int row;
    GtkWidget *pick_target;     /* entry filled by the next map click */
    MapPickFunc pick;           /* where map clicks go ... */
    gpointer pick_data;         /* ... and which dialog owns them */
} Frame;

static GtkWidget *
add_row(Frame *f, const char *label, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(label);

    gtk_label_set_xalign(GTK_LABEL(l), 1.0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
    gtk_grid_attach(GTK_GRID(f->grid), l, 0, f->row, 1, 1);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_grid_attach(GTK_GRID(f->grid), w, 1, f->row, 1, 1);
    f->row++;
    return w;
}

static GtkWidget *
sector_entry(Frame *f, const char *label, const char *tip)
{
    GtkWidget *e = gtk_entry_new();

    gtk_entry_set_placeholder_text(GTK_ENTRY(e), "x,y  or click the map");
    gtk_widget_set_tooltip_text(e, tip);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    return add_row(f, label, e);
}

static void
set_preview(Frame *f, const char *cmd)
{
    char *t = cmd ? g_strdup_printf("Command:  %s", cmd)
        : g_strdup("Command:  (fill in the fields)");

    gtk_label_set_text(GTK_LABEL(f->preview), t);
    gtk_widget_set_sensitive(f->ok, cmd != NULL && conn_state() == CONN_PLAYING);
    g_free(t);
}

static void
set_info(Frame *f, const char *text)
{
    gtk_label_set_text(GTK_LABEL(f->info), text ? text : "");
}

/* Remember which sector field had focus last; map clicks go there */
static gboolean
entry_focus(GtkWidget *w, GdkEvent *ev, gpointer f)
{
    ((Frame *)f)->pick_target = w;
    return FALSE;
}

static void
frame_new(Frame *f, const char *title, const char *ok_label)
{
    GtkWidget *box, *bb, *cancel;

    f->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(f->win), title);
    gtk_window_set_transient_for(GTK_WINDOW(f->win), mainwin_window());
    gtk_window_set_type_hint(GTK_WINDOW(f->win), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_resizable(GTK_WINDOW(f->win), FALSE);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_container_add(GTK_CONTAINER(f->win), box);
    f->grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(f->grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(f->grid), 8);
    gtk_box_pack_start(GTK_BOX(box), f->grid, FALSE, FALSE, 0);
    f->info = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(f->info), 0);
    gtk_label_set_line_wrap(GTK_LABEL(f->info), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(f->info), 50);
    gtk_label_set_selectable(GTK_LABEL(f->info), TRUE);
    gtk_widget_set_can_focus(f->info, FALSE);
    gtk_box_pack_start(GTK_BOX(box), f->info, FALSE, FALSE, 0);
    f->preview = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(f->preview), 0);
    gtk_label_set_selectable(GTK_LABEL(f->preview), TRUE);
    gtk_widget_set_can_focus(f->preview, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(f->preview),
                                "preview");
    gtk_box_pack_start(GTK_BOX(box), f->preview, FALSE, FALSE, 0);
    bb = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(bb), GTK_BUTTONBOX_END);
    gtk_box_set_spacing(GTK_BOX(bb), 6);
    cancel = gtk_button_new_with_mnemonic("_Close");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_widget_destroy),
                             f->win);
    gtk_container_add(GTK_CONTAINER(bb), cancel);
    f->ok = gtk_button_new_with_mnemonic(ok_label);
    gtk_widget_set_can_default(f->ok, TRUE);
    gtk_container_add(GTK_CONTAINER(bb), f->ok);
    gtk_box_pack_start(GTK_BOX(box), bb, FALSE, FALSE, 0);
    f->row = 0;
}

static gboolean
escape_closes(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    if (ev->keyval == GDK_KEY_Escape) {
        gtk_widget_destroy(w);
        return TRUE;
    }
    return FALSE;
}

/* map clicks go to whichever dialog was used last */
static gboolean
frame_focus(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    Frame *f = data;

    mapview_set_pick(f->pick, f->pick_data);
    return FALSE;
}

static void
frame_show(Frame *f, MapPickFunc pick, gpointer data)
{
    f->pick = pick;
    f->pick_data = data;
    g_signal_connect(f->win, "key-press-event", G_CALLBACK(escape_closes), NULL);
    g_signal_connect(f->win, "focus-in-event", G_CALLBACK(frame_focus), f);
    gtk_window_set_default(GTK_WINDOW(f->win), f->ok);
    gtk_widget_show_all(f->win);
    mapview_set_pick(pick, data);
}

static void
send(const char *cmd)
{
    conn_command(cmd, CMD_SHOW | CMD_USER, NULL, NULL, NULL);
}

static GtkWidget *
item_combo(void)
{
    GtkWidget *c = gtk_combo_box_text_new();

    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        char id[2] = { items[i].letter, 0 };

        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), id, items[i].label);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);
    return c;
}

static const Item *
combo_item(GtkWidget *c)
{
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(c));

    for (guint i = 0; id && i < G_N_ELEMENTS(items); i++)
        if (items[i].letter == id[0])
            return &items[i];
    return NULL;
}

/* ============================================================= move */

static struct {
    Frame f;
    GtkWidget *item, *amount, *from, *to, *show_path, *test;
    guint timer, gen;
    char *path;                 /* best path found, or NULL */
    double cost;                /* per unit */
    int total;                  /* total mobility cost, -1 if unknown */
    char *note;                 /* last thing test said, if no total */
} md;

typedef struct {
    guint gen;
    char *path, *note;
    double cost;
    int total;
} MoveQuery;

static void move_update(void);

static void
move_query_line(int id, const char *text, gpointer data)
{
    MoveQuery *q = data;
    const char *p;

    if (id != C_DATA)
        return;
    if (g_str_has_prefix(text, "Best path from ")
        && (p = strstr(text, " is "))) {
        const char *c = strstr(p, " (cost ");

        if (c) {
            g_free(q->path);
            q->path = g_strndup(p + 4, c - p - 4);
            q->cost = g_ascii_strtod(c + 7, NULL);
        }
    } else if ((p = strstr(text, "Total movement cost = "))) {
        q->total = atoi(p + 22);
    } else if (*text && !g_str_has_prefix(text, "Looking for")
               && !g_str_has_prefix(text, "Using best path")) {
        g_free(q->note);
        q->note = g_strdup(text);
    }
}

static void
move_query_done(gpointer data)
{
    MoveQuery *q = data;

    if (q->gen == md.gen && md.f.win) {
        g_free(md.path);
        md.path = q->path;
        q->path = NULL;
        md.cost = q->cost;
        md.total = q->total;
        g_free(md.note);
        md.note = q->note;
        q->note = NULL;
        move_update();
    }
    g_free(q->path);
    g_free(q->note);
    g_free(q);
}

/* Ask the server: the best path, and what the move would cost */
static gboolean
move_query(gpointer d)
{
    int fx, fy, tx, ty, n;
    char *to, *cmd;
    const Item *it;
    MoveQuery *q;

    md.timer = 0;
    md.gen++;
    g_clear_pointer(&md.path, g_free);
    g_clear_pointer(&md.note, g_free);
    md.total = -1;
    to = entry_trimmed(md.to);
    it = combo_item(md.item);
    n = spin_value(md.amount);
    if (conn_state() != CONN_PLAYING || !it
        || !parse_sector(entry_text(md.from), &fx, &fy) || !*to) {
        g_free(to);
        move_update();
        return G_SOURCE_REMOVE;
    }
    q = g_new0(MoveQuery, 1);
    q->gen = md.gen;
    q->total = -1;
    if (parse_sector(to, &tx, &ty)) {
        cmd = g_strdup_printf("bestpath %d,%d %d,%d", fx, fy, tx, ty);
        conn_command(cmd, 0, move_query_line, NULL, q);
        g_free(cmd);
        cmd = g_strdup_printf("test %c %d,%d %d %d,%d", it->letter, fx, fy,
                              MAX(n, 1), tx, ty);
    } else if (is_route(to)) {
        char *r = route_with_stop(to);

        q->path = g_strdup(to);
        cmd = g_strdup_printf("test %c %d,%d %d %s", it->letter, fx, fy,
                              MAX(n, 1), r);
        g_free(r);
    } else {
        g_free(q);
        g_free(to);
        move_update();
        return G_SOURCE_REMOVE;
    }
    conn_command(cmd, 0, move_query_line, move_query_done, q);
    g_free(cmd);
    g_free(to);
    move_update();
    return G_SOURCE_REMOVE;
}

static void
move_changed(void)
{
    if (md.timer)
        g_source_remove(md.timer);
    md.timer = g_timeout_add(400, move_query, NULL);
    move_update();
}

static char *
move_command(void)
{
    int fx, fy, tx, ty, n = spin_value(md.amount);
    const Item *it = combo_item(md.item);
    char *to = entry_trimmed(md.to), *cmd = NULL;

    if (it && n > 0 && parse_sector(entry_text(md.from), &fx, &fy)) {
        if (parse_sector(to, &tx, &ty))
            cmd = g_strdup_printf("move %c %d,%d %d %d,%d", it->letter, fx, fy,
                                  n, tx, ty);
        else if (is_route(to)) {
            char *r = route_with_stop(to);

            cmd = g_strdup_printf("move %c %d,%d %d %s", it->letter, fx, fy,
                                  n, r);
            g_free(r);
        }
    }
    g_free(to);
    return cmd;
}

static void
move_update(void)
{
    int fx, fy;
    const Item *it = combo_item(md.item);
    GString *info = g_string_new(NULL);
    char *cmd;

    if (it && parse_sector(entry_text(md.from), &fx, &fy)) {
        if (our_sector(fx, fy)) {
            int have = sect_value(fx, fy, it->col, 0);
            int mob = sect_value(fx, fy, "mobil", 0);

            g_string_append_printf(info, "%d,%d has %d %s and %d mobility.",
                                   fx, fy, have, it->label, mob);
            if (md.total >= 0)
                g_string_append_printf(info, "\nMobility cost %d, leaving %d.",
                                       md.total, mob - md.total);
        } else {
            g_string_append_printf(info, "%d,%d is not one of your sectors.",
                                   fx, fy);
        }
    }
    if (md.path)
        g_string_append_printf(info, "\nPath: %s", md.path);
    if (md.path && md.cost > 0)
        g_string_append_printf(info, "  (%.3f per unit)", md.cost);
    if (md.total < 0 && md.note)
        g_string_append_printf(info, "\n%s", md.note);
    if (md.timer)
        g_string_append(info, "\nChecking the path...");
    set_info(&md.f, info->str);
    g_string_free(info, TRUE);

    if (md.path && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(md.show_path))
        && parse_sector(entry_text(md.from), &fx, &fy))
        mapview_show_path(fx, fy, md.path);
    else
        mapview_show_path(0, 0, NULL);

    cmd = move_command();
    set_preview(&md.f, cmd);
    gtk_widget_set_sensitive(md.test, cmd != NULL
                             && conn_state() == CONN_PLAYING);
    g_free(cmd);
}

static void
move_ok(GtkButton *b, gpointer test)
{
    char *cmd = move_command();

    if (!cmd)
        return;
    if (test) {
        /* "test" takes the same arguments as "move" */
        char *t = g_strconcat("test", cmd + 4, NULL);

        send(t);
        g_free(t);
    } else {
        send(cmd);
        gtk_widget_destroy(md.f.win);
    }
    g_free(cmd);
}

static void
move_pick(int x, int y, gpointer d)
{
    char s[32];

    snprintf(s, sizeof(s), "%d,%d", x, y);
    if (md.f.pick_target == md.to || (*entry_text(md.from)
                                      && md.f.pick_target != md.from)) {
        gtk_entry_set_text(GTK_ENTRY(md.to), s);
    } else {
        gtk_entry_set_text(GTK_ENTRY(md.from), s);
        md.f.pick_target = md.to;
        gtk_widget_grab_focus(md.to);
    }
}

static void
move_destroyed(GtkWidget *w, gpointer d)
{
    if (md.timer)
        g_source_remove(md.timer);
    mapview_clear_pick(&md);
    mapview_show_path(0, 0, NULL);
    g_free(md.path);
    g_free(md.note);
    memset(&md, 0, sizeof(md));
}

void
dlg_move(int x, int y, gboolean have_sector)
{
    char s[32];

    if (!md.f.win) {
        frame_new(&md.f, "Move", "_Move");
        md.item = add_row(&md.f, "_Commodity:", item_combo());
        md.amount = add_row(&md.f, "_Amount:",
                            gtk_spin_button_new_with_range(0, 100000, 1));
        gtk_entry_set_activates_default(GTK_ENTRY(md.amount), TRUE);
        md.from = sector_entry(&md.f, "_From:", "The sector to move from");
        md.to = sector_entry(&md.f, "_To:", "The destination sector, or a "
                             "route of directions (yugjbn)");
        gtk_entry_set_placeholder_text(GTK_ENTRY(md.to),
                                       "x,y, a route like jjn, or click");
        md.show_path = gtk_check_button_new_with_mnemonic("Show path on _map");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(md.show_path), TRUE);
        gtk_grid_attach(GTK_GRID(md.f.grid), md.show_path, 1, md.f.row++, 1, 1);
        md.test = gtk_button_new_with_mnemonic("_Test");
        gtk_widget_set_tooltip_text(md.test, "Show what the move would do, "
                                    "without moving anything");
        gtk_container_add(GTK_CONTAINER(gtk_widget_get_parent(md.f.ok)),
                          md.test);
        gtk_box_reorder_child(GTK_BOX(gtk_widget_get_parent(md.f.ok)), md.test,
                              1);
        g_signal_connect_swapped(md.item, "changed", G_CALLBACK(move_changed),
                                 NULL);
        g_signal_connect_swapped(md.amount, "changed",
                                 G_CALLBACK(move_changed), NULL);
        g_signal_connect_swapped(md.from, "changed", G_CALLBACK(move_changed),
                                 NULL);
        g_signal_connect_swapped(md.to, "changed", G_CALLBACK(move_changed),
                                 NULL);
        g_signal_connect_swapped(md.show_path, "toggled",
                                 G_CALLBACK(move_update), NULL);
        g_signal_connect(md.from, "focus-in-event", G_CALLBACK(entry_focus),
                         &md.f);
        g_signal_connect(md.to, "focus-in-event", G_CALLBACK(entry_focus),
                         &md.f);
        g_signal_connect(md.f.ok, "clicked", G_CALLBACK(move_ok), NULL);
        g_signal_connect(md.test, "clicked", G_CALLBACK(move_ok),
                         GINT_TO_POINTER(1));
        g_signal_connect(md.f.win, "destroy", G_CALLBACK(move_destroyed), NULL);
        md.total = -1;
        frame_show(&md.f, move_pick, &md);
    }
    if (have_sector) {
        snprintf(s, sizeof(s), "%d,%d", x, y);
        gtk_entry_set_text(GTK_ENTRY(md.from), s);
        gtk_entry_set_text(GTK_ENTRY(md.to), "");
        md.f.pick_target = md.to;
        gtk_widget_grab_focus(md.to);
    }
    move_update();
    gtk_window_present(GTK_WINDOW(md.f.win));
    mapview_set_pick(move_pick, &md);
}

/* ===================================================== navigate/march */

static struct {
    Frame f;
    UnitClass cls;
    GtkWidget *units, *dest;
} nd;

static char *
nav_command(void)
{
    char *u = entry_trimmed(nd.units), *d = entry_trimmed(nd.dest), *cmd = NULL;
    const char *verb = nd.cls == UC_SHIP ? "navigate" : "march";
    int x, y;

    if (*u && !strchr(u, ' ')) {
        if (parse_sector(d, &x, &y))
            cmd = g_strdup_printf("%s %s %d,%d", verb, u, x, y);
        else if (is_route(d)) {
            char *r = route_with_stop(d);

            cmd = g_strdup_printf("%s %s %s", verb, u, r);
            g_free(r);
        }
    }
    g_free(u);
    g_free(d);
    return cmd;
}

static void
nav_update(void)
{
    char *u = entry_trimmed(nd.units), *d = entry_trimmed(nd.dest), *cmd;
    Unit *unit = find_unit(nd.cls, u);
    GString *info = g_string_new(NULL);
    int dx, dy;

    if (unit) {
        char *desc = world_unit_describe(unit);

        g_string_printf(info, "%s\nat %d,%d with %d mobility", desc, unit->x,
                        unit->y, unit->mob);
        g_free(desc);
        if (is_route(d))
            mapview_show_path(unit->x, unit->y, d);
        else
            mapview_show_path(0, 0, NULL);
    } else {
        mapview_show_path(0, 0, NULL);
        if (*u)
            g_string_printf(info, "Several %s, or a %s: %s",
                            world_unit_noun(nd.cls, TRUE),
                            nd.cls == UC_SHIP ? "fleet" : "army", u);
    }
    if (*d && !is_route(d) && !parse_sector(d, &dx, &dy))
        g_string_append(info, "\nThe destination should be a sector (x,y) "
                        "or directions (yugjbn).");
    set_info(&nd.f, info->str);
    g_string_free(info, TRUE);
    cmd = nav_command();
    set_preview(&nd.f, cmd);
    g_free(cmd);
    g_free(u);
    g_free(d);
}

static void
nav_ok(GtkButton *b, gpointer d)
{
    char *cmd = nav_command();

    if (cmd) {
        conn_command(cmd, CMD_SHOW | CMD_USER | CMD_STOP, NULL, NULL, NULL);
        g_free(cmd);
        gtk_widget_destroy(nd.f.win);
    }
}

/* A click on a sector with our units (when no units are chosen yet, or
 * the units field is in use) picks them; otherwise it is the
 * destination. */
static void
nav_pick(int x, int y, gpointer d)
{
    char *here = units_at(nd.cls, x, y);
    char s[32];

    if (*here && (!*entry_text(nd.units) || nd.f.pick_target == nd.units)) {
        gtk_entry_set_text(GTK_ENTRY(nd.units), here);
        nd.f.pick_target = nd.dest;
        gtk_widget_grab_focus(nd.dest);
    } else {
        snprintf(s, sizeof(s), "%d,%d", x, y);
        gtk_entry_set_text(GTK_ENTRY(nd.dest), s);
    }
    g_free(here);
}

static void
nav_destroyed(GtkWidget *w, gpointer d)
{
    mapview_clear_pick(&nd);
    mapview_show_path(0, 0, NULL);
    memset(&nd, 0, sizeof(nd));
}

void
dlg_nav(UnitClass c, const char *units)
{
    if (nd.f.win && nd.cls != c)
        gtk_widget_destroy(nd.f.win);
    if (!nd.f.win) {
        nd.cls = c;
        frame_new(&nd.f, c == UC_SHIP ? "Navigate" : "March",
                  c == UC_SHIP ? "_Navigate" : "_March");
        nd.units = add_row(&nd.f, c == UC_SHIP ? "_Ships:" : "_Land units:",
                           gtk_entry_new());
        gtk_entry_set_placeholder_text(GTK_ENTRY(nd.units), c == UC_SHIP
                                       ? "e.g. 3, 3/4/7, a fleet letter, "
                                       "or click" : "e.g. 0, 0/1, an army "
                                       "letter, or click");
        gtk_entry_set_activates_default(GTK_ENTRY(nd.units), TRUE);
        nd.dest = sector_entry(&nd.f, "_Destination:", "A sector to go to "
                               "(the server finds the way), or a route of "
                               "directions (yugjbn)");
        gtk_entry_set_placeholder_text(GTK_ENTRY(nd.dest),
                                       "x,y, a route like jjn, or click");
        g_signal_connect_swapped(nd.units, "changed", G_CALLBACK(nav_update),
                                 NULL);
        g_signal_connect_swapped(nd.dest, "changed", G_CALLBACK(nav_update),
                                 NULL);
        g_signal_connect(nd.units, "focus-in-event", G_CALLBACK(entry_focus),
                         &nd.f);
        g_signal_connect(nd.dest, "focus-in-event", G_CALLBACK(entry_focus),
                         &nd.f);
        g_signal_connect(nd.f.ok, "clicked", G_CALLBACK(nav_ok), NULL);
        g_signal_connect(nd.f.win, "destroy", G_CALLBACK(nav_destroyed), NULL);
        frame_show(&nd.f, nav_pick, &nd);
    }
    if (units) {
        gtk_entry_set_text(GTK_ENTRY(nd.units), units);
        nd.f.pick_target = nd.dest;
        gtk_widget_grab_focus(nd.dest);
    }
    nav_update();
    gtk_window_present(GTK_WINDOW(nd.f.win));
    mapview_set_pick(nav_pick, &nd);
}

/* ============================================================ build */

enum { B_SHIP, B_LAND, B_PLANE, B_NUKE, B_BRIDGE, B_TOWER, B_KINDS };

static struct {
    Frame f;
    GtkWidget *kind[B_KINDS], *sect, *type, *type_label, *number, *tech;
    GtkWidget *dir, *dir_label, *show;
    int tech_level;
} bd;

static const char *build_words[B_KINDS] = {
    "ship", "land", "plane", "nuke", "bridge", "tower"
};

static int
build_kind(void)
{
    for (int i = 0; i < B_KINDS; i++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd.kind[i])))
            return i;
    return B_SHIP;
}

/* Fill the type list from the server's characteristics table */
static void
build_fill_types(void)
{
    static const char *tables[] = { "ship-chr", "land-chr", "plane-chr",
                                    "nuke-chr" };
    int k = build_kind();
    sqlite3_stmt *st;
    char *sql;

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(bd.type));
    if (k > B_NUKE || !db_table_exists(tables[k]))
        return;
    sql = g_strdup_printf("SELECT name, l_build, h_build, bwork, tech, cost "
                          "FROM \"%s\" ORDER BY tech, type", tables[k]);
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 0);
            int tech = sqlite3_column_int(st, 4);
            char *mnem, *label;
            const char *sp;

            if (!name)
                continue;
            sp = strchr(name, ' ');
            mnem = sp ? g_strndup(name, sp - name) : g_strdup(name);
            label = g_strdup_printf("%s   lcm %d  hcm %d  avail %d  $%d%s",
                                    name, sqlite3_column_int(st, 1),
                                    sqlite3_column_int(st, 2),
                                    sqlite3_column_int(st, 3),
                                    sqlite3_column_int(st, 5),
                                    tech > bd.tech_level ? "  (needs tech" : "");
            if (tech > bd.tech_level) {
                char *t = g_strdup_printf("%s %d)", label, tech);

                g_free(label);
                label = t;
            }
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(bd.type), mnem, label);
            g_free(mnem);
            g_free(label);
        }
        sqlite3_finalize(st);
    }
    g_free(sql);
    gtk_combo_box_set_active(GTK_COMBO_BOX(bd.type), 0);
}

static char *
build_command(void)
{
    int k = build_kind(), x, y;
    char *sect = entry_trimmed(bd.sect), *cmd = NULL;

    if (parse_sector(sect, &x, &y)) {
        if (k >= B_BRIDGE) {
            const char *d = gtk_combo_box_get_active_id(GTK_COMBO_BOX(bd.dir));

            if (d)
                cmd = g_strdup_printf("build %s %d,%d %s", build_words[k], x, y,
                                      d);
        } else {
            const char *t = gtk_combo_box_get_active_id(GTK_COMBO_BOX(bd.type));
            int n = spin_value(bd.number);
            char *tech = entry_trimmed(bd.tech);

            if (t && n > 0) {
                if (*tech && atoi(tech) > 0)
                    cmd = g_strdup_printf("build %s %d,%d %s %d %d",
                                          build_words[k], x, y, t, n,
                                          atoi(tech));
                else
                    cmd = g_strdup_printf("build %s %d,%d %s %d",
                                          build_words[k], x, y, t, n);
            }
            g_free(tech);
        }
    }
    g_free(sect);
    return cmd;
}

static void
build_update(void)
{
    int k = build_kind(), x, y;
    gboolean bridge = k >= B_BRIDGE;
    GString *info = g_string_new(NULL);
    char *cmd;

    gtk_widget_set_visible(bd.type, !bridge);
    gtk_widget_set_visible(bd.type_label, !bridge);
    gtk_widget_set_sensitive(bd.number, !bridge);
    gtk_widget_set_sensitive(bd.tech, !bridge && k != B_NUKE);
    gtk_widget_set_visible(bd.dir, bridge);
    gtk_widget_set_visible(bd.dir_label, bridge);
    gtk_widget_set_sensitive(bd.show, k <= B_NUKE);
    if (parse_sector(entry_text(bd.sect), &x, &y)) {
        if (our_sector(x, y)) {
            Sector *s = world_sector(x, y);

            g_string_printf(info, "%s %d,%d: %d%% efficient, lcm %d, hcm %d, "
                            "avail %d, mil %d",
                            s && s->des ? world_des_name(s->des) : "sector", x, y,
                            sect_value(x, y, "effic", 0),
                            sect_value(x, y, "lcm", 0),
                            sect_value(x, y, "hcm", 0),
                            sect_value(x, y, "avail", 0),
                            sect_value(x, y, "milit", 0));
        } else {
            g_string_printf(info, "%d,%d is not one of your sectors.", x, y);
        }
    }
    if (bd.tech_level >= 0)
        g_string_append_printf(info, "%sYour tech level: %d", info->len ? "\n"
                               : "", bd.tech_level);
    set_info(&bd.f, info->str);
    g_string_free(info, TRUE);
    cmd = build_command();
    set_preview(&bd.f, cmd);
    g_free(cmd);
}

static void
build_kind_toggled(GtkToggleButton *b, gpointer d)
{
    if (gtk_toggle_button_get_active(b)) {
        build_fill_types();
        build_update();
    }
}

static void
build_ok(GtkButton *b, gpointer d)
{
    char *cmd = build_command();

    if (cmd) {
        send(cmd);
        g_free(cmd);
        gtk_widget_destroy(bd.f.win);
    }
}

static void
build_show(GtkButton *b, gpointer d)
{
    int k = build_kind();
    char *cmd;

    if (k > B_NUKE)
        return;
    cmd = g_strdup_printf("show %s build", build_words[k]);
    send(cmd);
    g_free(cmd);
}

static void
build_pick(int x, int y, gpointer d)
{
    char s[32];

    snprintf(s, sizeof(s), "%d,%d", x, y);
    gtk_entry_set_text(GTK_ENTRY(bd.sect), s);
}

static void
build_destroyed(GtkWidget *w, gpointer d)
{
    mapview_clear_pick(&bd);
    memset(&bd, 0, sizeof(bd));
}

void
dlg_build(int x, int y, gboolean have_sector)
{
    static const char *dirs[][2] = {
        { "y", "y  (up left)" }, { "u", "u  (up right)" }, { "g", "g  (left)" },
        { "j", "j  (right)" }, { "b", "b  (down left)" },
        { "n", "n  (down right)" },
    };
    static const char *kind_labels[B_KINDS] = {
        "_ship", "_land unit", "_plane", "n_uke", "bridge sp_an", "bridge _tower"
    };
    char s[32];

    if (!bd.f.win) {
        GtkWidget *kinds = gtk_grid_new();
        GSList *group = NULL;

        frame_new(&bd.f, "Build", "_Build");
        bd.tech_level = -1;
        if (db_table_exists("country") && my_cnum >= 0) {
            char *sql = g_strdup_printf("SELECT tech FROM country WHERE cnum=%d",
                                        my_cnum);

            bd.tech_level = db_get_int(sql, -1);
            g_free(sql);
        }
        gtk_grid_set_column_spacing(GTK_GRID(kinds), 8);
        for (int i = 0; i < B_KINDS; i++) {
            bd.kind[i] = gtk_radio_button_new_with_mnemonic(group,
                                                            kind_labels[i]);
            group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(bd.kind[i]));
            gtk_grid_attach(GTK_GRID(kinds), bd.kind[i], i % 3, i / 3, 1, 1);
        }
        add_row(&bd.f, "Build a:", kinds);
        bd.sect = sector_entry(&bd.f, "_In sector:", "Where to build: a "
                               "harbor for ships, headquarters for land units, "
                               "airfield for planes, nuclear plant for nukes");
        bd.type = gtk_combo_box_text_new();
        bd.type_label = gtk_label_new_with_mnemonic("T_ype:");
        gtk_label_set_xalign(GTK_LABEL(bd.type_label), 1.0);
        gtk_label_set_mnemonic_widget(GTK_LABEL(bd.type_label), bd.type);
        gtk_grid_attach(GTK_GRID(bd.f.grid), bd.type_label, 0, bd.f.row, 1, 1);
        gtk_grid_attach(GTK_GRID(bd.f.grid), bd.type, 1, bd.f.row++, 1, 1);
        bd.dir = gtk_combo_box_text_new();
        for (guint i = 0; i < G_N_ELEMENTS(dirs); i++)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(bd.dir), dirs[i][0],
                                      dirs[i][1]);
        bd.dir_label = gtk_label_new_with_mnemonic("_Direction:");
        gtk_label_set_xalign(GTK_LABEL(bd.dir_label), 1.0);
        gtk_label_set_mnemonic_widget(GTK_LABEL(bd.dir_label), bd.dir);
        gtk_grid_attach(GTK_GRID(bd.f.grid), bd.dir_label, 0, bd.f.row, 1, 1);
        gtk_grid_attach(GTK_GRID(bd.f.grid), bd.dir, 1, bd.f.row++, 1, 1);
        bd.number = add_row(&bd.f, "_Number:",
                            gtk_spin_button_new_with_range(1, 100, 1));
        bd.tech = add_row(&bd.f, "T_ech level:", gtk_entry_new());
        gtk_entry_set_placeholder_text(GTK_ENTRY(bd.tech),
                                       "optional: build below your tech");
        bd.show = gtk_button_new_with_mnemonic("_Show Build Info");
        gtk_widget_set_tooltip_text(bd.show, "List what each type needs");
        gtk_container_add(GTK_CONTAINER(gtk_widget_get_parent(bd.f.ok)),
                          bd.show);
        gtk_button_box_set_child_secondary(GTK_BUTTON_BOX(
            gtk_widget_get_parent(bd.f.ok)), bd.show, TRUE);
        for (int i = 0; i < B_KINDS; i++)
            g_signal_connect(bd.kind[i], "toggled",
                             G_CALLBACK(build_kind_toggled), NULL);
        g_signal_connect_swapped(bd.sect, "changed", G_CALLBACK(build_update),
                                 NULL);
        g_signal_connect_swapped(bd.type, "changed", G_CALLBACK(build_update),
                                 NULL);
        g_signal_connect_swapped(bd.dir, "changed", G_CALLBACK(build_update),
                                 NULL);
        g_signal_connect_swapped(bd.number, "changed",
                                 G_CALLBACK(build_update), NULL);
        g_signal_connect_swapped(bd.tech, "changed", G_CALLBACK(build_update),
                                 NULL);
        g_signal_connect(bd.f.ok, "clicked", G_CALLBACK(build_ok), NULL);
        g_signal_connect(bd.show, "clicked", G_CALLBACK(build_show), NULL);
        g_signal_connect(bd.f.win, "destroy", G_CALLBACK(build_destroyed), NULL);
        build_fill_types();
        frame_show(&bd.f, build_pick, &bd);
    }
    if (have_sector) {
        Sector *sec = world_sector(x, y);

        snprintf(s, sizeof(s), "%d,%d", x, y);
        gtk_entry_set_text(GTK_ENTRY(bd.sect), s);
        /* choose what this kind of sector builds */
        if (sec && sec->des == 'h')
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd.kind[B_SHIP]),
                                         TRUE);
        else if (sec && sec->des == '!')
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd.kind[B_LAND]),
                                         TRUE);
        else if (sec && sec->des == '*')
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd.kind[B_PLANE]),
                                         TRUE);
        else if (sec && sec->des == 'n')
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd.kind[B_NUKE]),
                                         TRUE);
        else if (sec && sec->des == '#')
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd.kind[B_BRIDGE]),
                                         TRUE);
    }
    build_update();
    gtk_window_present(GTK_WINDOW(bd.f.win));
    mapview_set_pick(build_pick, &bd);
}

/* ===================================================== load / unload */

enum { L_COMM, L_PLANES, L_LAND };

static struct {
    Frame f;
    UnitClass cls;
    GtkWidget *unload, *carrier, *what, *item, *item_label, *amount;
    GtkWidget *amount_label, *list, *list_label;
} ld;

static int
load_what(void)
{
    return gtk_combo_box_get_active(GTK_COMBO_BOX(ld.what));
}

static char *
load_command(void)
{
    char *u = entry_trimmed(ld.carrier), *cmd = NULL;
    gboolean unload = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ld.unload));
    const char *verb = ld.cls == UC_SHIP ? (unload ? "unload" : "load")
        : (unload ? "lunload" : "lload");
    int w = load_what();

    if (*u && !strchr(u, ' ')) {
        if (w == L_COMM) {
            const Item *it = combo_item(ld.item);
            int n = spin_value(ld.amount);

            if (it && n > 0)
                cmd = g_strdup_printf("%s %s %s %d", verb, it->name, u, n);
        } else {
            char *list = entry_trimmed(ld.list);

            if (*list && !strchr(list, ' '))
                cmd = g_strdup_printf("%s %s %s %s", verb,
                                      w == L_PLANES ? "plane" : "land", u, list);
            g_free(list);
        }
    }
    g_free(u);
    return cmd;
}

/* What the carrier has on board, from the database */
static void
load_cargo(GString *info, Unit *u)
{
    char *sql = g_strdup_printf("SELECT * FROM \"%s\" WHERE uid=%d",
                                world_unit_table(u->cls), u->uid);
    sqlite3_stmt *st;
    gboolean any = FALSE;

    if (sqlite3_prepare_v2(db_handle(), sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            int n = sqlite3_column_count(st);

            for (int i = 0; i < n; i++) {
                const char *col = sqlite3_column_name(st, i);

                for (guint j = 0; j < G_N_ELEMENTS(items); j++) {
                    int v;

                    if (strcmp(col, items[j].col))
                        continue;
                    v = sqlite3_column_int(st, i);
                    if (v) {
                        g_string_append_printf(info, "%s %d %s",
                                               any ? "," : "\nCarrying:", v,
                                               items[j].label);
                        any = TRUE;
                    }
                }
            }
        }
        sqlite3_finalize(st);
    }
    g_free(sql);
    if (!any)
        g_string_append(info, "\nCarrying no commodities.");
}

static void
load_update(void)
{
    int w = load_what();
    char *u = entry_trimmed(ld.carrier), *cmd;
    Unit *unit = find_unit(ld.cls, u);
    GString *info = g_string_new(NULL);

    gtk_widget_set_visible(ld.item, w == L_COMM);
    gtk_widget_set_visible(ld.item_label, w == L_COMM);
    gtk_widget_set_visible(ld.amount, w == L_COMM);
    gtk_widget_set_visible(ld.amount_label, w == L_COMM);
    gtk_widget_set_visible(ld.list, w != L_COMM);
    gtk_widget_set_visible(ld.list_label, w != L_COMM);
    if (unit) {
        char *desc = world_unit_describe(unit);

        g_string_printf(info, "%s at %d,%d", desc, unit->x, unit->y);
        g_free(desc);
        load_cargo(info, unit);
        if (w == L_COMM) {
            const Item *it = combo_item(ld.item);

            if (it && our_sector(unit->x, unit->y))
                g_string_append_printf(info, "\nThe sector has %d %s.",
                                       sect_value(unit->x, unit->y, it->col, 0),
                                       it->label);
        } else {
            char *here = units_at(w == L_PLANES ? UC_PLANE : UC_LAND, unit->x,
                                  unit->y);

            g_string_append_printf(info, "\n%s in the sector: %s",
                                   w == L_PLANES ? "Planes" : "Land units",
                                   *here ? here : "none");
            g_free(here);
        }
    }
    set_info(&ld.f, info->str);
    g_string_free(info, TRUE);
    cmd = load_command();
    set_preview(&ld.f, cmd);
    g_free(cmd);
    g_free(u);
}

static void
load_ok(GtkButton *b, gpointer d)
{
    char *cmd = load_command();

    if (cmd) {
        send(cmd);
        g_free(cmd);
        gtk_widget_destroy(ld.f.win);
    }
}

static void
load_pick(int x, int y, gpointer d)
{
    char *here = units_at(ld.cls, x, y);

    if (*here)
        gtk_entry_set_text(GTK_ENTRY(ld.carrier), here);
    g_free(here);
}

static void
load_destroyed(GtkWidget *w, gpointer d)
{
    mapview_clear_pick(&ld);
    memset(&ld, 0, sizeof(ld));
}

void
dlg_load(UnitClass c, const char *units)
{
    if (ld.f.win && ld.cls != c)
        gtk_widget_destroy(ld.f.win);
    if (!ld.f.win) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *load = gtk_radio_button_new_with_mnemonic(NULL, "_Load");

        ld.cls = c;
        frame_new(&ld.f, c == UC_SHIP ? "Load / Unload Ship"
                  : "Load / Unload Land Unit", "_OK");
        ld.unload = gtk_radio_button_new_with_mnemonic_from_widget(
            GTK_RADIO_BUTTON(load), "_Unload");
        gtk_container_add(GTK_CONTAINER(box), load);
        gtk_container_add(GTK_CONTAINER(box), ld.unload);
        add_row(&ld.f, "", box);
        ld.carrier = add_row(&ld.f, c == UC_SHIP ? "_Ships:" : "Land _units:",
                             gtk_entry_new());
        gtk_entry_set_placeholder_text(GTK_ENTRY(ld.carrier),
                                       "e.g. 1, 1/2, or click their sector");
        gtk_entry_set_activates_default(GTK_ENTRY(ld.carrier), TRUE);
        ld.what = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ld.what),
                                       "commodities");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ld.what), "planes");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ld.what),
                                       "land units");
        gtk_combo_box_set_active(GTK_COMBO_BOX(ld.what), 0);
        add_row(&ld.f, "_What:", ld.what);
        ld.item = item_combo();
        ld.item_label = gtk_label_new_with_mnemonic("_Commodity:");
        gtk_label_set_xalign(GTK_LABEL(ld.item_label), 1.0);
        gtk_label_set_mnemonic_widget(GTK_LABEL(ld.item_label), ld.item);
        gtk_grid_attach(GTK_GRID(ld.f.grid), ld.item_label, 0, ld.f.row, 1, 1);
        gtk_grid_attach(GTK_GRID(ld.f.grid), ld.item, 1, ld.f.row++, 1, 1);
        ld.amount = gtk_spin_button_new_with_range(0, 100000, 1);
        gtk_entry_set_activates_default(GTK_ENTRY(ld.amount), TRUE);
        ld.amount_label = gtk_label_new_with_mnemonic("_Amount:");
        gtk_label_set_xalign(GTK_LABEL(ld.amount_label), 1.0);
        gtk_label_set_mnemonic_widget(GTK_LABEL(ld.amount_label), ld.amount);
        gtk_grid_attach(GTK_GRID(ld.f.grid), ld.amount_label, 0, ld.f.row, 1, 1);
        gtk_grid_attach(GTK_GRID(ld.f.grid), ld.amount, 1, ld.f.row++, 1, 1);
        ld.list = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(ld.list), "e.g. 0/1/2");
        gtk_entry_set_activates_default(GTK_ENTRY(ld.list), TRUE);
        ld.list_label = gtk_label_new_with_mnemonic("_Numbers:");
        gtk_label_set_xalign(GTK_LABEL(ld.list_label), 1.0);
        gtk_label_set_mnemonic_widget(GTK_LABEL(ld.list_label), ld.list);
        gtk_grid_attach(GTK_GRID(ld.f.grid), ld.list_label, 0, ld.f.row, 1, 1);
        gtk_grid_attach(GTK_GRID(ld.f.grid), ld.list, 1, ld.f.row++, 1, 1);
        g_signal_connect_swapped(ld.unload, "toggled", G_CALLBACK(load_update),
                                 NULL);
        g_signal_connect_swapped(ld.carrier, "changed", G_CALLBACK(load_update),
                                 NULL);
        g_signal_connect_swapped(ld.what, "changed", G_CALLBACK(load_update),
                                 NULL);
        g_signal_connect_swapped(ld.item, "changed", G_CALLBACK(load_update),
                                 NULL);
        g_signal_connect_swapped(ld.amount, "changed",
                                 G_CALLBACK(load_update), NULL);
        g_signal_connect_swapped(ld.list, "changed", G_CALLBACK(load_update),
                                 NULL);
        g_signal_connect(ld.f.ok, "clicked", G_CALLBACK(load_ok), NULL);
        g_signal_connect(ld.f.win, "destroy", G_CALLBACK(load_destroyed), NULL);
        frame_show(&ld.f, load_pick, &ld);
    }
    if (units)
        gtk_entry_set_text(GTK_ENTRY(ld.carrier), units);
    load_update();
    gtk_window_present(GTK_WINDOW(ld.f.win));
    mapview_set_pick(load_pick, &ld);
}
