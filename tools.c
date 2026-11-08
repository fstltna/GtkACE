/*
 * tools.c - WinACE's tools (frmPromptProdSummary, frmToolHarbor,
 * frmToolFeeder, frmToolCheMarker, frmToolFortify, frmToolRange).
 *
 * Where WinACE worked figures out itself, GtkACE asks the server for
 * them (production, starvation, sstat, lstat, pstat), so they are exact
 * for the server's version and options.
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sqlite3.h>
#include "gtkace.h"

/* ---------------------------------------------------------- helpers */

typedef void (*LinesFunc)(GPtrArray *lines, gpointer data);

typedef struct {
    GPtrArray *lines;
    LinesFunc fn;
    gpointer data;
} Capture;

static void
capture_line(int id, const char *text, gpointer d)
{
    Capture *c = d;

    if (id == C_DATA)
        g_ptr_array_add(c->lines, emp_text_decode(text, TRUE, NULL));
}

static void
capture_done(gpointer d)
{
    Capture *c = d;

    c->fn(c->lines, c->data);
    g_ptr_array_unref(c->lines);
    g_free(c);
}

/* Run a command quietly and hand its output lines to fn */
static void
run_capture(const char *cmd, LinesFunc fn, gpointer data)
{
    Capture *c = g_new0(Capture, 1);

    c->lines = g_ptr_array_new_with_free_func(g_free);
    c->fn = fn;
    c->data = data;
    conn_command(cmd, 0, capture_line, capture_done, c);
}

static gboolean
need_connection(void)
{
    if (conn_state() == CONN_PLAYING)
        return TRUE;
    mainwin_error("Not connected", "This tool asks the server for figures; "
                  "connect to the game first.");
    return FALSE;
}

static gboolean
parse_xy(const char *s, int *x, int *y)
{
    char *end;
    long a, b;

    if (!s)
        return FALSE;
    while (*s == ' ')
        s++;
    a = strtol(s, &end, 10);
    if (end == s || *end != ',')
        return FALSE;
    s = end + 1;
    b = strtol(s, &end, 10);
    if (end == s)
        return FALSE;
    *x = a;
    *y = b;
    return TRUE;
}

/* Our sectors' values, keyed "x,y" -> GHashTable of column -> int */
static int
sect_int(int x, int y, const char *col, int dflt)
{
    sqlite3_stmt *st;
    char *sql;
    int v = dflt;

    if (!db_table_exists("sect"))
        return dflt;
    sql = g_strdup_printf("SELECT \"%s\" FROM sect WHERE xloc=? AND yloc=?", col);
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, x);
        sqlite3_bind_int(st, 2, y);
        if (sqlite3_step(st) == SQLITE_ROW
            && sqlite3_column_type(st, 0) != SQLITE_NULL)
            v = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    g_free(sql);
    return v;
}

/* The commodities, by their one-letter names */
static const struct { char letter; const char *name, *col; } items[] = {
    { 'c', "civilians", "civil" }, { 'm', "military", "milit" },
    { 'u', "uw", "uw" }, { 'f', "food", "food" }, { 's', "shells", "shell" },
    { 'g', "guns", "gun" }, { 'p', "petrol", "petrol" }, { 'i', "iron", "iron" },
    { 'd', "dust", "dust" }, { 'b', "bars", "bar" }, { 'o', "oil", "oil" },
    { 'l', "lcm", "lcm" }, { 'h', "hcm", "hcm" }, { 'r', "rads", "rad" },
};

static int
item_index(char letter)
{
    for (guint i = 0; i < G_N_ELEMENTS(items); i++)
        if (items[i].letter == letter)
            return i;
    return -1;
}

/* Numbers sort as numbers ("96%", "3,-1" by row) */
static gboolean
num(const char *s, double *v)
{
    char *end;
    double d;

    if (!s)
        return FALSE;
    while (*s == ' ' || *s == '$' || *s == '~')
        s++;
    d = g_ascii_strtod(s, &end);
    if (end == s)
        return FALSE;
    if (*end == ',') {
        double y = g_ascii_strtod(end + 1, &end);

        d = y * 100000 + d;
    }
    if (*end == '%')
        end++;
    *v = d;
    return !*end;
}

static int
sort_cells(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer col)
{
    char *sa, *sb;
    double va, vb;
    int r;

    gtk_tree_model_get(m, a, GPOINTER_TO_INT(col), &sa, -1);
    gtk_tree_model_get(m, b, GPOINTER_TO_INT(col), &sb, -1);
    if (num(sa, &va) && num(sb, &vb))
        r = va < vb ? -1 : va > vb;
    else
        r = g_strcmp0(sa, sb);
    g_free(sa);
    g_free(sb);
    return r;
}

/* A sortable list of strings with the given headings.  A heading
 * starting with "?" is a tick box column (G_TYPE_BOOLEAN). */
static GtkWidget *
make_list(GtkListStore **storep, const char *const *heads, int n)
{
    GType *types = g_new(GType, n);
    GtkListStore *store;
    GtkWidget *tv;

    for (int i = 0; i < n; i++)
        types[i] = heads[i][0] == '?' ? G_TYPE_BOOLEAN : G_TYPE_STRING;
    store = gtk_list_store_newv(n, types);
    g_free(types);
    tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tv), FALSE);
    for (int i = 0; i < n; i++) {
        GtkTreeViewColumn *col;

        if (heads[i][0] == '?') {
            GtkCellRenderer *r = gtk_cell_renderer_toggle_new();

            g_object_set_data(G_OBJECT(r), "col", GINT_TO_POINTER(i));
            g_object_set_data(G_OBJECT(r), "store", store);
            col = gtk_tree_view_column_new_with_attributes(heads[i] + 1, r,
                                                           "active", i, NULL);
        } else {
            GtkCellRenderer *r = gtk_cell_renderer_text_new();

            g_object_set(r, "family", "Monospace", NULL);
            col = gtk_tree_view_column_new_with_attributes(heads[i], r, "text",
                                                           i, NULL);
            gtk_tree_view_column_set_sort_column_id(col, i);
            gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(store), i,
                                            sort_cells, GINT_TO_POINTER(i),
                                            NULL);
        }
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }
    *storep = store;
    return tv;
}

static void
toggle_cell(GtkCellRendererToggle *r, char *path, gpointer d)
{
    GtkListStore *store = g_object_get_data(G_OBJECT(r), "store");
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "col"));
    GtkTreeIter it;
    gboolean v;

    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &it, path))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &it, col, &v, -1);
    gtk_list_store_set(store, &it, col, !v, -1);
}

/* Make tick boxes in a list clickable */
static void
connect_toggles(GtkWidget *tv)
{
    GList *cols = gtk_tree_view_get_columns(GTK_TREE_VIEW(tv));

    for (GList *l = cols; l; l = l->next) {
        GList *cells = gtk_cell_layout_get_cells(l->data);

        for (GList *c = cells; c; c = c->next)
            if (GTK_IS_CELL_RENDERER_TOGGLE(c->data))
                g_signal_connect(c->data, "toggled", G_CALLBACK(toggle_cell),
                                 NULL);
        g_list_free(cells);
    }
    g_list_free(cols);
}

static GtkWidget *
scrolled(GtkWidget *child, int min_height)
{
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);

    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    if (min_height > 0)
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw),
                                                   min_height);
    gtk_container_add(GTK_CONTAINER(sw), child);
    return sw;
}

/* Double-clicking a row finds the sector in its first x,y cell */
static void
find_row_on_map(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *c,
                gpointer d)
{
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    GtkTreeIter it;
    int n = gtk_tree_model_get_n_columns(m);

    if (!gtk_tree_model_get_iter(m, &it, path))
        return;
    for (int k = 0; k < n; k++) {
        char *s;
        int x, y;

        if (gtk_tree_model_get_column_type(m, k) != G_TYPE_STRING)
            continue;
        gtk_tree_model_get(m, &it, k, &s, -1);
        if (parse_xy(s, &x, &y) && !strchr(s, ' ')) {
            g_free(s);
            mapview_center(x, y);
            mapview_select(x, y);
            gtk_window_present(mainwin_window());
            return;
        }
        g_free(s);
    }
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

static GtkWidget *
tool_window(GtkWidget **winp, const char *title, int w, int h)
{
    GtkWidget *box;

    *winp = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(*winp), title);
    gtk_window_set_transient_for(GTK_WINDOW(*winp), mainwin_window());
    gtk_window_set_default_size(GTK_WINDOW(*winp), w, h);
    g_signal_connect(*winp, "destroy", G_CALLBACK(gtk_widget_destroyed), winp);
    g_signal_connect(*winp, "key-press-event", G_CALLBACK(escape_closes), NULL);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(*winp), box);
    return box;
}

static GtkWidget *
note_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);

    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    return l;
}

static void
set_status(GtkWidget *l, const char *fmt, ...)
{
    va_list ap;
    char *t;

    if (!l)
        return;
    va_start(ap, fmt);
    t = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(l), t);
    g_free(t);
}

/* Words of a line with their positions */
typedef struct { int s, e; char *w; } Word;

static GArray *
split_words(const char *l)
{
    GArray *a = g_array_new(FALSE, FALSE, sizeof(Word));

    for (int c = 0; l[c];) {
        Word w;

        while (l[c] == ' ')
            c++;
        if (!l[c])
            break;
        w.s = c;
        while (l[c] && l[c] != ' ')
            c++;
        w.e = c;
        w.w = g_strndup(l + w.s, w.e - w.s);
        g_array_append_val(a, w);
    }
    return a;
}

static void
free_words(GArray *a)
{
    for (guint i = 0; i < a->len; i++)
        g_free(g_array_index(a, Word, i).w);
    g_array_free(a, TRUE);
}

/* Which heading a value belongs under: Empire right-aligns numbers, so
 * the heading whose end is nearest the value's end */
static const char *
heading_for(GArray *heads, const Word *v)
{
    const char *best = NULL;
    int bd = G_MAXINT;

    for (guint i = 0; i < heads->len; i++) {
        Word *h = &g_array_index(heads, Word, i);
        int ov = MIN(h->e, v->e) - MAX(h->s, v->s);
        int d = ov > 0 ? -ov : abs(h->e - v->e);

        if (d < bd) {
            bd = d;
            best = h->w;
        }
    }
    return best;
}

/* ================================================ production summary */

static struct {
    GtkWidget *win, *center, *food, *short_, *idle, *mark, *status;
    GtkListStore *sum, *probs;
    double made[G_N_ELEMENTS(items)], used[G_N_ELEMENTS(items)];
    gboolean all;
    int cx, cy;
} ps;

static gboolean
ps_wanted(int x, int y)
{
    return ps.all || (sect_int(x, y, "xdist", x) == ps.cx
                      && sect_int(x, y, "ydist", y) == ps.cy);
}

static void
ps_problem(int x, int y, const char *what)
{
    GtkTreeIter it;
    char s[32];
    Sector *sec = world_sector(x, y);
    char d[2] = { sec && sec->des ? sec->des : '?', 0 };

    snprintf(s, sizeof(s), "%d,%d", x, y);
    gtk_list_store_append(ps.probs, &it);
    gtk_list_store_set(ps.probs, &it, 0, s, 1, d, 2, what, -1);
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ps.mark)))
        mapview_add_marker(x, y, MK_PROBLEM, what);
}

/* "  1,-1   m  96%   400  384i 1.00 $0          ...      384" */
static void
ps_production(GPtrArray *lines, gpointer d)
{
    GArray *heads = NULL;

    if (!ps.win)
        return;
    for (guint i = 0; i < lines->len; i++) {
        const char *l = g_ptr_array_index(lines, i);
        GArray *w;
        int x, y, make = -1, max = -1;
        char made_item = 0;

        if (strstr(l, "sect") && strstr(l, "make")) {
            if (heads)
                free_words(heads);
            heads = split_words(l);
            continue;
        }
        if (!heads || !parse_xy(l, &x, &y))
            continue;
        w = split_words(l);
        for (guint k = 0; k < w->len; k++) {
            Word *v = &g_array_index(w, Word, k);
            const char *h = heading_for(heads, v);
            size_t n = strlen(v->w);
            char letter = n ? v->w[n - 1] : 0;
            int amount = atoi(v->w);

            if (!h)
                continue;
            if (!strcmp(h, "make") && g_ascii_isalpha(letter)) {
                make = amount;
                made_item = letter;
            } else if (g_str_has_prefix(h, "use") && g_ascii_isalpha(letter)) {
                int idx = item_index(letter);

                if (idx >= 0 && ps_wanted(x, y))
                    ps.used[idx] += amount;
            } else if (!strcmp(h, "max") && !g_ascii_isalpha(letter)) {
                max = amount;
            }
        }
        free_words(w);
        if (!ps_wanted(x, y))
            continue;
        if (made_item && item_index(made_item) >= 0)
            ps.made[item_index(made_item)] += MAX(make, 0);
        if (make == 0 && max == 0
            && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ps.idle)))
            ps_problem(x, y, "makes nothing");
        else if (make >= 0 && max > make
                 && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ps.short_))) {
            char *t = g_strdup_printf("short of materials: makes %d of %d",
                                      make, max);

            ps_problem(x, y, t);
            g_free(t);
        }
    }
    if (heads)
        free_words(heads);
}

/* "  -1,-3   -    100% will starve 510 people. 31 more food needed" */
static void
ps_starvation(GPtrArray *lines, gpointer d)
{
    GtkTreeIter it;

    if (!ps.win)
        return;
    for (guint i = 0; i < lines->len; i++) {
        const char *l = g_ptr_array_index(lines, i), *p;
        int x, y;

        if (!parse_xy(l, &x, &y) || !(p = strstr(l, "will starve"))
            || !ps_wanted(x, y))
            continue;
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ps.food))) {
            const char *n = strstr(p, ". ");
            char *t = n ? g_strdup_printf("starving: %s", n + 2)
                : g_strdup("starving");

            ps_problem(x, y, t);
            g_free(t);
        }
    }
    /* the commodity summary */
    gtk_list_store_clear(ps.sum);
    for (guint k = 0; k < G_N_ELEMENTS(items); k++) {
        int have = 0;
        char *sql, a[32], b[32], c[32], e[32];

        sql = g_strdup_printf("SELECT \"%s\", xloc, yloc FROM sect", items[k].col);
        {
            sqlite3_stmt *st;

            if (db_table_exists("sect")
                && sqlite3_prepare_v2(db_handle(), sql, -1, &st, NULL)
                == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW)
                    if (ps_wanted(sqlite3_column_int(st, 1),
                                  sqlite3_column_int(st, 2)))
                        have += sqlite3_column_int(st, 0);
                sqlite3_finalize(st);
            }
        }
        g_free(sql);
        snprintf(a, sizeof(a), "%d", have);
        snprintf(b, sizeof(b), "%.0f", ps.made[k]);
        snprintf(c, sizeof(c), "%.0f", ps.used[k]);
        snprintf(e, sizeof(e), "%+.0f", ps.made[k] - ps.used[k]);
        gtk_list_store_append(ps.sum, &it);
        gtk_list_store_set(ps.sum, &it, 0, items[k].name, 1, a, 2, b, 3, c,
                           4, e, -1);
    }
    set_status(ps.status, "%d problems found.  Double-click a row to find "
               "it on the map.",
               gtk_tree_model_iter_n_children(GTK_TREE_MODEL(ps.probs), NULL));
}

static void
ps_run(void)
{
    const char *c = gtk_entry_get_text(GTK_ENTRY(ps.center));

    if (!need_connection())
        return;
    ps.all = !*c;
    if (!ps.all && !parse_xy(c, &ps.cx, &ps.cy)) {
        mainwin_error("Distribution sector", "Enter a sector as x,y, or leave "
                      "it empty for all sectors.");
        return;
    }
    memset(ps.made, 0, sizeof(ps.made));
    memset(ps.used, 0, sizeof(ps.used));
    gtk_list_store_clear(ps.probs);
    mapview_clear_markers(MK_PROBLEM);
    set_status(ps.status, "Asking the server...");
    run_capture("production *", ps_production, NULL);
    run_capture("starvation *", ps_starvation, NULL);
}

void
tool_production(void)
{
    static const char *sum_heads[] = { "Commodity", "On hand", "Made",
                                       "Used", "Change" };
    static const char *prob_heads[] = { "Sector", "Des", "Problem" };
    GtkWidget *box, *row, *b, *tv, *pane;

    if (ps.win) {
        gtk_window_present(GTK_WINDOW(ps.win));
        return;
    }
    box = tool_window(&ps.win, "Production Summary", 640, 560);
    gtk_box_pack_start(GTK_BOX(box), note_label(
        "What the next update will make and use, from the server's "
        "production report, and the sectors with problems."), FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Distribution sector:"),
                       FALSE, FALSE, 0);
    ps.center = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ps.center), "all sectors");
    gtk_entry_set_width_chars(GTK_ENTRY(ps.center), 12);
    gtk_box_pack_start(GTK_BOX(row), ps.center, FALSE, FALSE, 0);
    b = gtk_button_new_with_mnemonic("_Run");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(ps_run), NULL);
    gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    ps.food = gtk_check_button_new_with_label("Food shortage");
    ps.short_ = gtk_check_button_new_with_label("Short of materials");
    ps.idle = gtk_check_button_new_with_label("Making nothing");
    ps.mark = gtk_check_button_new_with_label("Mark them on the map");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps.food), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps.short_), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps.idle), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps.mark), TRUE);
    gtk_container_add(GTK_CONTAINER(row), gtk_label_new("Report:"));
    gtk_container_add(GTK_CONTAINER(row), ps.food);
    gtk_container_add(GTK_CONTAINER(row), ps.short_);
    gtk_container_add(GTK_CONTAINER(row), ps.idle);
    gtk_container_add(GTK_CONTAINER(row), ps.mark);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    pane = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    tv = make_list(&ps.sum, sum_heads, 5);
    gtk_paned_pack1(GTK_PANED(pane), scrolled(tv, 150), FALSE, FALSE);
    tv = make_list(&ps.probs, prob_heads, 3);
    g_signal_connect(tv, "row-activated", G_CALLBACK(find_row_on_map), NULL);
    gtk_paned_pack2(GTK_PANED(pane), scrolled(tv, 120), TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(pane), 260);
    gtk_box_pack_start(GTK_BOX(box), pane, TRUE, TRUE, 0);
    ps.status = note_label("");
    gtk_box_pack_start(GTK_BOX(box), ps.status, FALSE, FALSE, 0);
    gtk_widget_show_all(ps.win);
    if (conn_state() == CONN_PLAYING)
        ps_run();
}

/* ================================================== build projection */

static struct {
    GtkWidget *win, *kind, *sects_tv, *types_tv, *status;
    GtkListStore *sects, *types;
    int x, y;
    gboolean have;
} bp;

static const struct { const char *label, *table; char des; } bp_kinds[] = {
    { "ships (harbors)", "ship-chr", 'h' },
    { "land units (headquarters)", "land-chr", '!' },
    { "planes (airfields)", "plane-chr", '*' },
    { "nukes (nuclear plants)", "nuke-chr", 'n' },
};

static double
country_value(const char *col)
{
    char *sql;
    double v = 0;
    sqlite3_stmt *st;

    if (!db_table_exists("country") || my_cnum < 0)
        return 0;
    sql = g_strdup_printf("SELECT \"%s\" FROM country WHERE cnum=%d", col,
                          my_cnum);
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            v = sqlite3_column_double(st, 0);
        sqlite3_finalize(st);
    }
    g_free(sql);
    return v;
}

static void
bp_fill_types(void)
{
    int k = gtk_combo_box_get_active(GTK_COMBO_BOX(bp.kind));
    sqlite3_stmt *st;
    char *sql;
    double money = country_value("money"), tech = country_value("tech");
    int avail = bp.have ? sect_int(bp.x, bp.y, "avail", 0) : 0;

    gtk_list_store_clear(bp.types);
    if (!bp.have || k < 0 || !db_table_exists(bp_kinds[k].table))
        return;
    sql = g_strdup_printf("SELECT * FROM \"%s\" ORDER BY tech",
                          bp_kinds[k].table);
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &st, NULL) == SQLITE_OK) {
        int n = sqlite3_column_count(st);

        while (sqlite3_step(st) == SQLITE_ROW) {
            int can = G_MAXINT, need_tech = 0, cost = 0, work = 0;
            GString *needs = g_string_new(NULL), *short_ = g_string_new(NULL);
            const char *name = "";
            GtkTreeIter it;
            char a[16], b[16], w[16];

            for (int i = 0; i < n; i++) {
                const char *col = sqlite3_column_name(st, i);
                int v = sqlite3_column_int(st, i);

                if (!strcmp(col, "name"))
                    name = (const char *)sqlite3_column_text(st, i);
                else if (!strcmp(col, "tech"))
                    need_tech = v;
                else if (!strcmp(col, "cost"))
                    cost = v;
                else if (!strcmp(col, "bwork"))
                    work = v;
                else if (strlen(col) == 7 && g_str_has_suffix(col, "_build")
                         && v > 0) {
                    int idx = item_index(col[0]);
                    int have;

                    if (idx < 0)
                        continue;
                    have = sect_int(bp.x, bp.y, items[idx].col, 0);
                    g_string_append_printf(needs, "%s%d %s", needs->len ? ", "
                                           : "", v, items[idx].col);
                    can = MIN(can, have / v);
                    if (have < v)
                        g_string_append_printf(short_, "%s%d %s",
                                               short_->len ? ", " : "", v - have,
                                               items[idx].col);
                }
            }
            if (work > 0) {
                can = MIN(can, avail / work);
                if (avail < work)
                    g_string_append_printf(short_, "%s%d avail",
                                           short_->len ? ", " : "",
                                           work - avail);
            }
            if (cost > 0) {
                can = MIN(can, (int)(money / cost));
                if (money < cost)
                    g_string_append_printf(short_, "%s$%.0f",
                                           short_->len ? ", " : "",
                                           cost - money);
            }
            if (can == G_MAXINT)
                can = 0;
            if (need_tech > tech) {
                can = 0;
                g_string_printf(short_, "needs tech %d", need_tech);
            }
            snprintf(a, sizeof(a), "%d", can);
            snprintf(b, sizeof(b), "$%d", cost);
            if (work)
                snprintf(w, sizeof(w), "%d", work);
            else
                w[0] = 0;
            gtk_list_store_append(bp.types, &it);
            gtk_list_store_set(bp.types, &it, 0, name ? name : "", 1, needs->str,
                               2, w, 3, b,
                               4, a, 5, short_->str, -1);
            g_string_free(needs, TRUE);
            g_string_free(short_, TRUE);
        }
        sqlite3_finalize(st);
    }
    g_free(sql);
}

static void
bp_fill_sects(void)
{
    int k = gtk_combo_box_get_active(GTK_COMBO_BOX(bp.kind));

    gtk_list_store_clear(bp.sects);
    gtk_list_store_clear(bp.types);
    bp.have = FALSE;
    if (k < 0 || world_x <= 0)
        return;
    for (int y = -world_y / 2; y < world_y / 2; y++)
        for (int x = -world_x / 2; x < world_x / 2; x++) {
            Sector *s = world_sector(x, y);
            GtkTreeIter it;
            char c[32], e[16], l[16], h[16], a[16];

            if (!s || s->des != bp_kinds[k].des || s->own != my_cnum)
                continue;
            snprintf(c, sizeof(c), "%d,%d", x, y);
            snprintf(e, sizeof(e), "%d%%", sect_int(x, y, "effic", 0));
            snprintf(l, sizeof(l), "%d", sect_int(x, y, "lcm", 0));
            snprintf(h, sizeof(h), "%d", sect_int(x, y, "hcm", 0));
            snprintf(a, sizeof(a), "%d", sect_int(x, y, "avail", 0));
            gtk_list_store_append(bp.sects, &it);
            gtk_list_store_set(bp.sects, &it, 0, c, 1, e, 2, l, 3, h, 4, a, -1);
        }
    {
        GtkTreeIter it;

        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(bp.sects), &it))
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(
                                               GTK_TREE_VIEW(bp.sects_tv)), &it);
    }
    set_status(bp.status, "Money $%.0f, tech %.0f.  Can build counts use the "
               "sector's lcm, hcm and avail, and your money.",
               country_value("money"), country_value("tech"));
}

static void
bp_sect_selected(GtkTreeSelection *sel, gpointer d)
{
    GtkTreeModel *m;
    GtkTreeIter it;
    char *s;

    bp.have = FALSE;
    if (gtk_tree_selection_get_selected(sel, &m, &it)) {
        gtk_tree_model_get(m, &it, 0, &s, -1);
        bp.have = parse_xy(s, &bp.x, &bp.y);
        g_free(s);
    }
    bp_fill_types();
}

static void
bp_build(void)
{
    if (bp.have)
        dlg_build(bp.x, bp.y, TRUE);
}

void
tool_build_projection(void)
{
    static const char *sect_heads[] = { "Sector", "Eff", "Lcm", "Hcm", "Avail" };
    static const char *type_heads[] = { "Type", "Materials", "Work", "Cost",
                                        "Can build", "Short of" };
    GtkWidget *box, *row, *b, *pane;

    if (bp.win) {
        gtk_window_present(GTK_WINDOW(bp.win));
        return;
    }
    box = tool_window(&bp.win, "Build Projection", 820, 520);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Build:"), FALSE, FALSE, 0);
    bp.kind = gtk_combo_box_text_new();
    for (guint i = 0; i < G_N_ELEMENTS(bp_kinds); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(bp.kind),
                                       bp_kinds[i].label);
    gtk_box_pack_start(GTK_BOX(row), bp.kind, FALSE, FALSE, 0);
    b = gtk_button_new_with_mnemonic("_Build Here...");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(bp_build), NULL);
    gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    pane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    bp.sects_tv = make_list(&bp.sects, sect_heads, 5);
    g_signal_connect(bp.sects_tv, "row-activated", G_CALLBACK(find_row_on_map),
                     NULL);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(bp.sects_tv)),
                     "changed", G_CALLBACK(bp_sect_selected), NULL);
    gtk_paned_pack1(GTK_PANED(pane), scrolled(bp.sects_tv, 0), FALSE, FALSE);
    bp.types_tv = make_list(&bp.types, type_heads, 6);
    gtk_paned_pack2(GTK_PANED(pane), scrolled(bp.types_tv, 0), TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(pane), 260);
    gtk_box_pack_start(GTK_BOX(box), pane, TRUE, TRUE, 0);
    bp.status = note_label("");
    gtk_box_pack_start(GTK_BOX(box), bp.status, FALSE, FALSE, 0);
    g_signal_connect_swapped(bp.kind, "changed", G_CALLBACK(bp_fill_sects),
                             NULL);
    gtk_combo_box_set_active(GTK_COMBO_BOX(bp.kind), 0);
    gtk_widget_show_all(bp.win);
}

/* ===================================================== famine relief */

enum { FR_DO, FR_SECT, FR_NEED, FR_FROM, FR_DIST, FR_AMOUNT, FR_NCOLS };

static struct {
    GtkWidget *win, *margin, *range, *keep, *status, *tv;
    GtkListStore *store;
} fr;

typedef struct {
    int x, y, spare;
} Donor;

static void
fr_starvation(GPtrArray *lines, gpointer d)
{
    GArray *donors = g_array_new(FALSE, FALSE, sizeof(Donor));
    GHashTable *starving = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, NULL);
    double margin, keep;
    int range, n = 0, unmet = 0;
    sqlite3_stmt *st;

    if (!fr.win) {
        g_array_free(donors, TRUE);
        g_hash_table_destroy(starving);
        return;
    }
    margin = gtk_spin_button_get_value(GTK_SPIN_BUTTON(fr.margin)) / 100.0;
    keep = gtk_spin_button_get_value(GTK_SPIN_BUTTON(fr.keep)) / 100.0;
    range = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(fr.range));
    gtk_list_store_clear(fr.store);
    mapview_clear_markers(MK_STARVE);

    for (guint i = 0; i < lines->len; i++) {
        const char *l = g_ptr_array_index(lines, i);
        int x, y;

        if (parse_xy(l, &x, &y) && strstr(l, "will starve"))
            g_hash_table_add(starving, g_strdup_printf("%d,%d", x, y));
    }
    /* sectors with food to spare: not starving, keeping a share */
    if (db_table_exists("sect")
        && sqlite3_prepare_v2(db_handle(), "SELECT xloc, yloc, food, f_dist "
                              "FROM sect WHERE food > 0", -1, &st, NULL)
        == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            Donor dn = { sqlite3_column_int(st, 0), sqlite3_column_int(st, 1), 0 };
            char key[32];
            int food = sqlite3_column_int(st, 2), th = sqlite3_column_int(st, 3);

            snprintf(key, sizeof(key), "%d,%d", dn.x, dn.y);
            if (g_hash_table_contains(starving, key))
                continue;
            dn.spare = food - MAX(th, (int)ceil(food * keep));
            if (dn.spare > 0)
                g_array_append_val(donors, dn);
        }
        sqlite3_finalize(st);
    }
    /* the nearest donor that can cover each need */
    for (guint i = 0; i < lines->len; i++) {
        const char *l = g_ptr_array_index(lines, i), *p;
        int x, y, need, best = -1, bd = G_MAXINT, amount;
        char sect[32], from[32] = "", dist[16] = "", amt[16], needs[16];
        GtkTreeIter it;

        if (!parse_xy(l, &x, &y) || !strstr(l, "will starve"))
            continue;
        p = strstr(l, ". ");
        need = p ? atoi(p + 2) : 0;
        amount = (int)ceil(need * (1 + margin));
        for (guint k = 0; k < donors->len; k++) {
            Donor *dn = &g_array_index(donors, Donor, k);
            int dd = world_distance(x, y, dn->x, dn->y);

            if (dn->spare >= amount && dd <= range && dd < bd) {
                bd = dd;
                best = k;
            }
        }
        snprintf(sect, sizeof(sect), "%d,%d", x, y);
        snprintf(needs, sizeof(needs), "%d", need);
        snprintf(amt, sizeof(amt), "%d", amount);
        if (best >= 0) {
            Donor *dn = &g_array_index(donors, Donor, best);

            dn->spare -= amount;
            snprintf(from, sizeof(from), "%d,%d", dn->x, dn->y);
            snprintf(dist, sizeof(dist), "%d", bd);
        } else {
            unmet++;
        }
        gtk_list_store_append(fr.store, &it);
        gtk_list_store_set(fr.store, &it, FR_DO, best >= 0, FR_SECT, sect,
                           FR_NEED, needs, FR_FROM, best >= 0 ? from : "none near",
                           FR_DIST, dist, FR_AMOUNT, amt, -1);
        mapview_add_marker(x, y, MK_STARVE, l + strspn(l, " "));
        n++;
    }
    if (!n)
        set_status(fr.status, "No sectors will starve at the next update.");
    else if (unmet)
        set_status(fr.status, "%d sectors will starve; no food nearby for %d.  "
                   "Tick the ones to help, then Move Food or Set Thresholds.",
                   n, unmet);
    else
        set_status(fr.status, "%d sectors will starve.  Tick the ones to "
                   "help, then Move Food or Set Thresholds.", n);
    g_array_free(donors, TRUE);
    g_hash_table_destroy(starving);
}

static void
fr_check(void)
{
    if (!need_connection())
        return;
    set_status(fr.status, "Asking the server...");
    run_capture("starvation *", fr_starvation, NULL);
}

/* Act on the ticked rows: move the food, or raise the food threshold so
 * distribution brings it */
static void
fr_act(GtkWidget *w, gpointer thresholds)
{
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(fr.store), &it);
    int sent = 0;

    if (!need_connection())
        return;
    while (ok) {
        gboolean go;
        char *sect, *from, *amount, *cmd = NULL;
        int x, y;

        gtk_tree_model_get(GTK_TREE_MODEL(fr.store), &it, FR_DO, &go,
                           FR_SECT, &sect, FR_FROM, &from, FR_AMOUNT, &amount,
                           -1);
        if (go && thresholds)
            cmd = g_strdup_printf("threshold f %s %s", sect, amount);
        else if (go && parse_xy(from, &x, &y))
            cmd = g_strdup_printf("move f %s %s %s", from, amount, sect);
        if (cmd) {
            conn_command(cmd, CMD_SHOW | CMD_USER | CMD_STOP, NULL, NULL, NULL);
            g_free(cmd);
            sent++;
        }
        g_free(sect);
        g_free(from);
        g_free(amount);
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(fr.store), &it);
    }
    set_status(fr.status, sent ? "%d commands sent; see the output." :
               "Tick the sectors to help first.", sent);
}

void
tool_famine(void)
{
    static const char *heads[] = { "?Help", "Sector", "Food needed", "From",
                                   "Distance", "Amount" };
    GtkWidget *box, *g, *b, *row;

    if (fr.win) {
        gtk_window_present(GTK_WINDOW(fr.win));
        return;
    }
    box = tool_window(&fr.win, "Famine Relief", 620, 480);
    gtk_box_pack_start(GTK_BOX(box), note_label(
        "Finds the sectors that will starve at the next update (the server's "
        "starvation report) and the nearest sectors with food to spare."),
        FALSE, FALSE, 0);
    g = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(g), 8);
    gtk_grid_set_row_spacing(GTK_GRID(g), 4);
    fr.margin = gtk_spin_button_new_with_range(0, 500, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(fr.margin), 10);
    fr.range = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(fr.range), 8);
    fr.keep = gtk_spin_button_new_with_range(0, 100, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(fr.keep), 50);
    gtk_grid_attach(GTK_GRID(g), note_label("Send this much more than "
                                            "needed (%):"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), fr.margin, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), note_label("Take food from sectors within "
                                            "(sectors):"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(g), fr.range, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(g), note_label("Leave them at least (% of their "
                                            "food):"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(g), fr.keep, 1, 2, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), g, FALSE, FALSE, 0);
    {
        GtkWidget *tv = make_list(&fr.store, heads, FR_NCOLS);

        connect_toggles(tv);
        g_signal_connect(tv, "row-activated", G_CALLBACK(find_row_on_map),
                         NULL);
        fr.tv = tv;
        gtk_box_pack_start(GTK_BOX(box), scrolled(tv, 160), TRUE, TRUE, 0);
    }
    fr.status = note_label("");
    gtk_box_pack_start(GTK_BOX(box), fr.status, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    b = gtk_button_new_with_mnemonic("_Check Starvation");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(fr_check), NULL);
    gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
    b = gtk_button_new_with_mnemonic("Set _Thresholds");
    gtk_widget_set_tooltip_text(b, "Raise each ticked sector's food threshold "
                                "so distribution brings the food");
    g_signal_connect(b, "clicked", G_CALLBACK(fr_act), GINT_TO_POINTER(1));
    gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
    b = gtk_button_new_with_mnemonic("_Move Food");
    gtk_widget_set_tooltip_text(b, "Move the food now from the sector shown");
    g_signal_connect(b, "clicked", G_CALLBACK(fr_act), NULL);
    gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_widget_show_all(fr.win);
    if (conn_state() == CONN_PLAYING)
        fr_check();
}

/* ===================================================== che and plague */

static struct {
    GtkWidget *win, *days, *status;
    GtkListStore *store;
} cm;

static int
event_kind(const char *line)
{
    char *l = g_ascii_strdown(line, -1);
    int k = -1;

    if (strstr(l, "plague"))
        k = MK_PLAGUE;
    else if (strstr(l, "revolutionary") || strstr(l, "rebel")
             || strstr(l, "guerrilla") || strstr(l, "partisan")
             || strstr(l, "retaken") || strstr(l, "che "))
        k = MK_CHE;
    else if (strstr(l, "starved"))
        k = MK_STARVE;
    g_free(l);
    return k;
}

static void
cm_scan(void)
{
    static const char *names[MK_COUNT] = { "problem", "che", "plague",
                                           "starvation" };
    sqlite3_stmt *st;
    gint64 since = g_get_real_time() / G_USEC_PER_SEC
        - gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(cm.days)) * 86400;
    int n = 0;

    gtk_list_store_clear(cm.store);
    if (!db_table_exists("telegrams")
        || sqlite3_prepare_v2(db_handle(), "SELECT dated, body FROM telegrams "
                              "WHERE kind <> 'Sent' AND received >= ? ORDER BY id",
                              -1, &st, NULL) != SQLITE_OK) {
        set_status(cm.status, "No telegrams yet: check for new telegrams "
                   "first.");
        return;
    }
    sqlite3_bind_int64(st, 1, since);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *dated = (const char *)sqlite3_column_text(st, 0);
        char **lines = g_strsplit((const char *)sqlite3_column_text(st, 1), "\n",
                                  -1);

        for (int i = 0; lines[i]; i++) {
            int kind = event_kind(lines[i]);

            if (kind < 0)
                continue;
            /* every sector named on the line */
            for (const char *p = lines[i]; *p; p++) {
                int x, y;
                char *end, sect[32];
                GtkTreeIter it;

                if (!(g_ascii_isdigit(*p) || (*p == '-' && g_ascii_isdigit(p[1])))
                    || (p > lines[i] && (g_ascii_isdigit(p[-1]) || p[-1] == '-'
                                         || p[-1] == ',')))
                    continue;
                if (!parse_xy(p, &x, &y))
                    continue;
                strtol(p, &end, 10);
                strtol(end + 1, &end, 10);
                p = end - 1;
                snprintf(sect, sizeof(sect), "%d,%d", x, y);
                gtk_list_store_append(cm.store, &it);
                gtk_list_store_set(cm.store, &it, 0, sect, 1, names[kind],
                                   2, g_strstrip(lines[i]), 3, dated, -1);
                mapview_add_marker(x, y, kind, lines[i]);
                n++;
            }
        }
        g_strfreev(lines);
    }
    sqlite3_finalize(st);
    set_status(cm.status, n ? "%d events marked on the map.  Hover over a "
               "marked sector to read its note." : "Nothing found in the "
               "telegrams of that period.", n);
}

static void
cm_clear(void)
{
    mapview_clear_markers(MK_CHE);
    mapview_clear_markers(MK_PLAGUE);
    mapview_clear_markers(MK_STARVE);
    gtk_list_store_clear(cm.store);
    set_status(cm.status, "Markers cleared.");
}

void
tool_che_plague(void)
{
    static const char *heads[] = { "Sector", "Kind", "Message", "Dated" };
    GtkWidget *box, *row, *b, *tv;

    if (cm.win) {
        gtk_window_present(GTK_WINDOW(cm.win));
        return;
    }
    box = tool_window(&cm.win, "Che and Plague Markers", 760, 420);
    gtk_box_pack_start(GTK_BOX(box), note_label(
        "Marks the sectors that your bulletins and production reports say "
        "have rebels (che), plague or starvation."), FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("From the last"), FALSE,
                       FALSE, 0);
    cm.days = gtk_spin_button_new_with_range(1, 365, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(cm.days), 3);
    gtk_box_pack_start(GTK_BOX(row), cm.days, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("days of telegrams"), FALSE,
                       FALSE, 0);
    b = gtk_button_new_with_mnemonic("_Clear Markers");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(cm_clear), NULL);
    gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
    b = gtk_button_new_with_mnemonic("_Mark on Map");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(cm_scan), NULL);
    gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    tv = make_list(&cm.store, heads, 4);
    g_signal_connect(tv, "row-activated", G_CALLBACK(find_row_on_map), NULL);
    gtk_box_pack_start(GTK_BOX(box), scrolled(tv, 160), TRUE, TRUE, 0);
    cm.status = note_label("");
    gtk_box_pack_start(GTK_BOX(box), cm.status, FALSE, FALSE, 0);
    gtk_widget_show_all(cm.win);
    cm_scan();
}

/* =========================================================== fortify */

enum { FT_DO, FT_UNIT, FT_TYPE, FT_SECT, FT_MOB, FT_AMOUNT, FT_NCOLS };

static struct {
    GtkWidget *win, *all, *status, *settings;
    GtkListStore *store;
    int max, gain;
} ft;

static double
version_value(const char *col, double dflt)
{
    char *sql;
    double v = dflt;
    sqlite3_stmt *st;

    if (!db_table_exists("version"))
        return dflt;
    sql = g_strdup_printf("SELECT \"%s\" FROM version", col);
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW
            && sqlite3_column_type(st, 0) != SQLITE_NULL)
            v = sqlite3_column_double(st, 0);
        sqlite3_finalize(st);
    }
    g_free(sql);
    return v;
}

static void
ft_fill(void)
{
    GPtrArray *a = world_units(UC_LAND);
    gboolean all = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ft.all));
    int n = 0;

    ft.max = (int)version_value("land_mob_max", 127);
    ft.gain = (int)round(version_value("etu_per_update", 60)
                         * version_value("land_grow_scale", 1.0));
    set_status(ft.settings, "Land units hold at most %d mobility and gain %d "
               "each update, so any above %d is lost at the update.", ft.max,
               ft.gain, MAX(ft.max - ft.gain, 0));
    gtk_list_store_clear(ft.store);
    for (guint i = 0; i < a->len; i++) {
        Unit *u = g_ptr_array_index(a, i);
        int lose = MAX(0, u->mob + ft.gain - ft.max);
        int amount = all ? u->mob : lose;
        char un[16], sect[32], mob[16], amt[16];
        GtkTreeIter it;

        snprintf(un, sizeof(un), "%d", u->uid);
        snprintf(sect, sizeof(sect), "%d,%d", u->x, u->y);
        snprintf(mob, sizeof(mob), "%d", u->mob);
        snprintf(amt, sizeof(amt), "%d", amount);
        gtk_list_store_append(ft.store, &it);
        gtk_list_store_set(ft.store, &it, FT_DO, amount > 0, FT_UNIT, un,
                           FT_TYPE, world_unit_type_name(UC_LAND, u->type),
                           FT_SECT, sect, FT_MOB, mob, FT_AMOUNT, amt, -1);
        if (amount > 0)
            n++;
    }
    set_status(ft.status, a->len ? "%d of %u land units %s." : "You have no "
               "land units.", n, a->len, all ? "have mobility to use"
               : "would lose mobility at the update");
}

static void
ft_go(void)
{
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ft.store), &it);
    int sent = 0;

    if (!need_connection())
        return;
    while (ok) {
        gboolean go;
        char *un, *amt;

        gtk_tree_model_get(GTK_TREE_MODEL(ft.store), &it, FT_DO, &go,
                           FT_UNIT, &un, FT_AMOUNT, &amt, -1);
        if (go && atoi(amt) > 0) {
            char *cmd = g_strdup_printf("fortify %s %s", un, amt);

            conn_command(cmd, CMD_SHOW | CMD_USER, NULL, NULL, NULL);
            g_free(cmd);
            sent++;
        }
        g_free(un);
        g_free(amt);
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(ft.store), &it);
    }
    set_status(ft.status, sent ? "%d fortify commands sent." : "Nothing ticked "
               "to fortify.", sent);
}

void
tool_fortify(void)
{
    static const char *heads[] = { "?Fortify", "Unit", "Type", "Sector",
                                   "Mobility", "Use" };
    GtkWidget *box, *row, *b, *tv;

    if (ft.win) {
        gtk_window_present(GTK_WINDOW(ft.win));
        return;
    }
    box = tool_window(&ft.win, "Fortify", 560, 400);
    ft.settings = note_label("");
    gtk_box_pack_start(GTK_BOX(box), ft.settings, FALSE, FALSE, 0);
    ft.all = gtk_check_button_new_with_mnemonic(
        "Use _all mobility, not just what would be lost");
    g_signal_connect_swapped(ft.all, "toggled", G_CALLBACK(ft_fill), NULL);
    gtk_box_pack_start(GTK_BOX(box), ft.all, FALSE, FALSE, 0);
    tv = make_list(&ft.store, heads, FT_NCOLS);
    connect_toggles(tv);
    g_signal_connect(tv, "row-activated", G_CALLBACK(find_row_on_map), NULL);
    gtk_box_pack_start(GTK_BOX(box), scrolled(tv, 160), TRUE, TRUE, 0);
    ft.status = note_label("");
    gtk_box_pack_start(GTK_BOX(box), ft.status, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    b = gtk_button_new_with_mnemonic("_Refresh");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(ft_fill), NULL);
    gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
    b = gtk_button_new_with_mnemonic("_Fortify");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(ft_go), NULL);
    gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    ft_fill();
    gtk_widget_show_all(ft.win);
}

/* ======================================================= range table */

static const struct { const char *cmd, *title, *range_col; } rt_kinds[] = {
    { "sstat *", "Ships", "rng" },
    { "lstat *", "Land units", "rng" },
    { "pstat *", "Planes", "ran" },
};

static struct {
    GtkWidget *win, *nb, *status;
    GtkWidget *tv[3];
    GtkListStore *store[3];
    char **heads[3];
} rt;

/* "r n g" (headings written downwards) -> "rng" */
static char *
squeeze(const char *s)
{
    GString *g = g_string_new(NULL);

    for (; *s; s++)
        if (*s != ' ')
            g_string_append_c(g, *s);
    return g_string_free(g, FALSE);
}

static void
rt_table(GPtrArray *lines, gpointer d)
{
    int k = GPOINTER_TO_INT(d);
    char **heads = NULL;
    GPtrArray *rows;
    GtkWidget *sw;
    int n;

    if (!rt.win)
        return;
    rows = report_parse_table(lines, &heads);
    if (rt.tv[k]) {
        gtk_widget_destroy(gtk_widget_get_parent(rt.tv[k]));
        rt.tv[k] = NULL;
    }
    if (rt.heads[k])
        g_strfreev(rt.heads[k]);
    rt.heads[k] = NULL;
    if (!rows) {
        GtkWidget *l = note_label("None.");

        gtk_container_add(GTK_CONTAINER(gtk_notebook_get_nth_page(
                                            GTK_NOTEBOOK(rt.nb), k)), l);
        gtk_widget_show(l);
        return;
    }
    n = g_strv_length(heads);
    for (int i = 0; i < n; i++) {
        char *s = squeeze(heads[i]);

        g_free(heads[i]);
        heads[i] = s;
    }
    rt.heads[k] = heads;
    rt.tv[k] = make_list(&rt.store[k], (const char *const *)heads, n);
    for (guint r = 0; r < rows->len; r++) {
        char **cells = g_ptr_array_index(rows, r);
        GtkTreeIter it;

        gtk_list_store_append(rt.store[k], &it);
        for (int c = 0; c < n && cells[c]; c++)
            gtk_list_store_set(rt.store[k], &it, c, cells[c], -1);
    }
    g_ptr_array_unref(rows);
    g_signal_connect(rt.tv[k], "row-activated", G_CALLBACK(find_row_on_map),
                     NULL);
    sw = scrolled(rt.tv[k], 0);
    {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(rt.nb), k);
        GList *kids = gtk_container_get_children(GTK_CONTAINER(page));

        for (GList *l = kids; l; l = l->next)
            gtk_widget_destroy(l->data);
        g_list_free(kids);
        gtk_box_pack_start(GTK_BOX(page), sw, TRUE, TRUE, 0);
    }
    gtk_widget_show_all(sw);
}

static void
rt_load(void)
{
    if (!need_connection())
        return;
    for (int k = 0; k < 3; k++)
        run_capture(rt_kinds[k].cmd, rt_table, GINT_TO_POINTER(k));
    set_status(rt.status, "Select a unit and press Show Range to shade the "
               "sectors it can reach.");
}

static int
column_named(int k, const char *name)
{
    for (int i = 0; rt.heads[k] && rt.heads[k][i]; i++)
        if (!strcmp(rt.heads[k][i], name))
            return i;
    return -1;
}

static void
rt_show(void)
{
    int k = gtk_notebook_get_current_page(GTK_NOTEBOOK(rt.nb));
    GtkTreeModel *m;
    GtkTreeIter it;
    int cr, cxy, x, y, range;
    double fire = 0;
    char *rs = NULL, *xy = NULL;

    if (k < 0 || k > 2 || !rt.tv[k]
        || !gtk_tree_selection_get_selected(gtk_tree_view_get_selection(
                                                GTK_TREE_VIEW(rt.tv[k])), &m, &it)) {
        set_status(rt.status, "Select a unit first.");
        return;
    }
    cr = column_named(k, rt_kinds[k].range_col);
    cxy = column_named(k, "x,y");
    if (cr < 0 || cxy < 0) {
        set_status(rt.status, "This server's report has no %s column.",
                   rt_kinds[k].range_col);
        return;
    }
    gtk_tree_model_get(m, &it, cr, &rs, cxy, &xy, -1);
    range = rs ? atoi(rs) : 0;
    if (k < 2) {
        /* sstat and lstat show the unit's range figure; the guns reach
         * half that, scaled by tech (effrange() in the server) */
        int ct = column_named(k, "tech");
        char *ts = NULL;
        int tech = 0;

        if (ct >= 0) {
            gtk_tree_model_get(m, &it, ct, &ts, -1);
            tech = ts ? atoi(ts) : 0;
            g_free(ts);
        }
        fire = range / 2.0 * (50.0 + tech) / (200.0 + tech)
            * version_value("fire_range_factor", 1.0);
        range = (int)floor(fire + 0.5);
    }
    if (parse_xy(xy, &x, &y)) {
        mapview_center(x, y);
        mapview_select(x, y);
        mapview_show_range(x, y, range);
        if (k == 2)
            set_status(rt.status, "Range %d: it can fly %d sectors out, or %d "
                       "out and back.", range, range, range / 2);
        else
            set_status(rt.status, "Its guns reach %.2f sectors from %d,%d "
                       "(%d, or %d when the server rounds up).", fire, x, y,
                       (int)floor(fire), (int)ceil(fire));
    }
    g_free(rs);
    g_free(xy);
}

static void
rt_clear(void)
{
    mapview_show_range(0, 0, -1);
}

static void
rt_destroyed(GtkWidget *w, gpointer d)
{
    for (int k = 0; k < 3; k++)
        if (rt.heads[k])
            g_strfreev(rt.heads[k]);
    memset(&rt, 0, sizeof(rt));
    mapview_show_range(0, 0, -1);
}

void
tool_range(void)
{
    GtkWidget *box, *row, *b;

    if (rt.win) {
        gtk_window_present(GTK_WINDOW(rt.win));
        return;
    }
    box = tool_window(&rt.win, "Range Table", 760, 420);
    g_signal_connect(rt.win, "destroy", G_CALLBACK(rt_destroyed), NULL);
    gtk_box_pack_start(GTK_BOX(box), note_label(
        "Your units' statistics at their tech level, from the server "
        "(sstat, lstat, pstat).  Show Range shades the sectors a unit's guns "
        "reach, or a plane can fly to."), FALSE, FALSE, 0);
    rt.nb = gtk_notebook_new();
    for (int k = 0; k < 3; k++)
        gtk_notebook_append_page(GTK_NOTEBOOK(rt.nb),
                                 gtk_box_new(GTK_ORIENTATION_VERTICAL, 0),
                                 gtk_label_new(rt_kinds[k].title));
    gtk_box_pack_start(GTK_BOX(box), rt.nb, TRUE, TRUE, 0);
    rt.status = note_label("");
    gtk_box_pack_start(GTK_BOX(box), rt.status, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    b = gtk_button_new_with_mnemonic("_Refresh");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(rt_load), NULL);
    gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
    b = gtk_button_new_with_mnemonic("C_lear Range");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(rt_clear), NULL);
    gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
    b = gtk_button_new_with_mnemonic("_Show Range on Map");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(rt_show), NULL);
    gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_widget_show_all(rt.win);
    rt_load();
}
