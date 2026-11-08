/*
 * reports.c - the report window (WinACE frmReport).
 *
 * The output of report commands (census, budget, nation, ship ...) is
 * shown in a Reports window, one tab per report, as well as in the main
 * output.  Empire's tables are also turned into sortable tables;
 * double-click a row to find its sector on the map.
 */
#include <string.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include "gtkace.h"

/* Report commands and their titles.  Abbreviations of three letters or
 * more are recognised, as Empire does, when they are not ambiguous. */
static const struct { const char *cmd, *title; gboolean table; } kinds[] = {
    { "budget", "Budget", FALSE }, { "census", "Census", TRUE },
    { "commodity", "Commodities", TRUE }, { "coastwatch", "Coastwatch", FALSE },
    { "country", "Countries", TRUE }, { "cutoff", "Cutoff", TRUE },
    { "level", "Distribution Levels", TRUE }, { "nation", "Nation", FALSE },
    { "news", "Newspaper", FALSE }, { "power", "Power", TRUE },
    { "production", "Production", TRUE }, { "report", "Tech Report", TRUE },
    { "resource", "Resources", TRUE }, { "show", "Show", FALSE },
    { "starvation", "Starvation", TRUE }, { "update", "Updates", FALSE },
    { "version", "Version", FALSE }, { "relations", "Relations", FALSE },
    { "players", "Players", FALSE }, { "ship", "Ships", TRUE },
    { "land", "Land Units", TRUE }, { "plane", "Planes", TRUE },
    { "nuke", "Nukes", TRUE }, { "cargo", "Ship Cargo", TRUE },
    { "lcargo", "Land Unit Cargo", TRUE }, { "sstat", "Ship Stats", TRUE },
    { "lstat", "Land Unit Stats", TRUE }, { "pstat", "Plane Stats", TRUE },
    { "list", "Commands", FALSE }, { "info", "Info", FALSE },
    { "ledger", "Loans", FALSE }, { "financial", "Financial", FALSE },
    { "market", "Market", TRUE }, { "motd", "Message of the Day", FALSE },
    { "map", "Map", FALSE }, { "bmap", "Bmap", FALSE }, { "nmap", "Nmap", FALSE },
    { "smap", "Smap", FALSE }, { "lmap", "Lmap", FALSE }, { "pmap", "Pmap", FALSE },
    { "lbmap", "Land Unit Bmap", FALSE }, { "pbmap", "Plane Bmap", FALSE },
    { "sbmap", "Ship Bmap", FALSE }, { "route", "Routes", FALSE },
    { "neweff", "New Efficiency", TRUE }, { "dump", "Dump", TRUE },
};

typedef struct {
    int kind;
    char *cmd;                  /* last command, for Refresh */
    GPtrArray *lines;
    GtkWidget *page, *stack, *text, *tree, *label;
    gboolean has_table;
} Report;

static struct {
    int capturing;              /* kind, or -1 */
    char *cmd;
    GPtrArray *lines;
    GtkWidget *win, *notebook, *table_btn, *status;
    GPtrArray *reports;         /* Report * by kind order of creation */
} rp = { -1 };

/* --------------------------------------------------------- matching */

static int
report_kind(const char *cmd)
{
    char word[32];
    size_t n;
    int found = -1;

    if (sscanf(cmd, " %31[a-zA-Z]", word) != 1)
        return -1;
    n = strlen(word);
    for (char *p = word; *p; p++)
        *p = g_ascii_tolower(*p);
    for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) {
        if (!strcmp(kinds[i].cmd, word))
            return i;           /* exact match wins */
        if (n >= 3 && !strncmp(kinds[i].cmd, word, n)) {
            if (found >= 0)
                return -1;      /* ambiguous */
            found = i;
        }
    }
    return found;
}

gboolean
reports_enabled(void)
{
    char *v = db_get_setting("reports_window");
    gboolean on = !v || atoi(v);

    g_free(v);
    return on;
}

void
reports_set_enabled(gboolean on)
{
    db_set_setting("reports_window", on ? "1" : "0");
}

/* -------------------------------------------------------- the table */

/* A data row starts with a sector ("-1,3") or a number ("12"), and has
 * at least three words. */
static gboolean
is_data_row(const char *l)
{
    const char *p = l + strspn(l, " ");
    char *end;
    int words = 0;
    gboolean in = FALSE;

    strtol(p, &end, 10);
    if (end == p)
        return FALSE;
    if (*end == ',') {
        const char *q = end + 1;

        strtol(q, &end, 10);
        if (end == q)
            return FALSE;
    }
    if (*end && *end != ' ')
        return FALSE;
    for (; *p; p++) {
        if (*p != ' ' && !in)
            words++;
        in = *p != ' ';
    }
    return words >= 3;
}

static gboolean
is_date_line(const char *l)
{
    static const char *days[] = { "Mon ", "Tue ", "Wed ", "Thu ", "Fri ",
                                  "Sat ", "Sun " };

    for (guint i = 0; i < G_N_ELEMENTS(days); i++)
        if (g_str_has_prefix(l, days[i]))
            return TRUE;
    return FALSE;
}

/* A title word over a table ("CENSUS"), not a column heading */
static gboolean
is_title_word(const char *w, size_t n)
{
    if (n < 4)
        return FALSE;
    for (size_t i = 0; i < n; i++)
        if (!g_ascii_isupper(w[i]))
            return FALSE;
    return TRUE;
}

typedef struct {
    int ncols;
    int *start, *end;           /* character ranges of the columns */
    char **heads;               /* column headings */
    int first, last;            /* data rows: lines[first..last] */
} Table;

static void
table_free(Table *t)
{
    if (!t)
        return;
    g_free(t->start);
    g_free(t->end);
    if (t->heads)
        g_strfreev(t->heads);
    g_free(t);
}

static Table *
find_table(GPtrArray *lines)
{
    Table *t;
    guint i;
    int first = -1, last = -1, width = 0;
    char *blank;
    GArray *st, *en;

    /* the first run of data rows */
    for (i = 0; i < lines->len; i++) {
        const char *l = g_ptr_array_index(lines, i);

        if (is_data_row(l)) {
            if (first < 0)
                first = i;
            last = i;
        } else if (first >= 0) {
            break;
        }
    }
    if (first < 0 || last - first < 1)
        return NULL;
    for (i = first; i <= (guint)last; i++)
        width = MAX(width, (int)strlen(g_ptr_array_index(lines, i)));
    /* a column boundary is a position blank in every row */
    blank = g_malloc(width + 1);
    memset(blank, 1, width + 1);
    for (i = first; i <= (guint)last; i++) {
        const char *l = g_ptr_array_index(lines, i);

        for (int c = 0; l[c]; c++)
            if (l[c] != ' ')
                blank[c] = 0;
    }
    st = g_array_new(FALSE, FALSE, sizeof(int));
    en = g_array_new(FALSE, FALSE, sizeof(int));
    for (int c = 0; c < width;) {
        int s, e;

        while (c < width && blank[c])
            c++;
        if (c >= width)
            break;
        s = c;
        while (c < width && !blank[c])
            c++;
        e = c;
        g_array_append_val(st, s);
        g_array_append_val(en, e);
    }
    g_free(blank);
    if (st->len < 3) {
        g_array_free(st, TRUE);
        g_array_free(en, TRUE);
        return NULL;
    }
    t = g_new0(Table, 1);
    t->ncols = st->len;
    t->start = (int *)g_array_free(st, FALSE);
    t->end = (int *)g_array_free(en, FALSE);
    t->first = first;
    t->last = last;
    t->heads = g_new0(char *, t->ncols + 1);

    /* headings: words on the (up to three) lines above the rows, each
     * given to the column it overlaps most, or else the nearest */
    for (int h = MAX(0, first - 3); h < first; h++) {
        const char *l = g_ptr_array_index(lines, h);

        if (!*l || is_date_line(l))
            continue;
        for (int c = 0; l[c];) {
            int s, e, best = -1, best_score = G_MININT;

            while (l[c] == ' ')
                c++;
            if (!l[c])
                break;
            s = c;
            while (l[c] && l[c] != ' ')
                c++;
            e = c;
            if (is_title_word(l + s, e - s))
                continue;
            for (int k = 0; k < t->ncols; k++) {
                int ov = MIN(e, t->end[k]) - MAX(s, t->start[k]);
                int score = ov > 0 ? ov * 1000
                    : -abs((s + e) / 2 - (t->start[k] + t->end[k]) / 2);

                if (score > best_score) {
                    best_score = score;
                    best = k;
                }
            }
            if (best >= 0) {
                char *w = g_strndup(l + s, e - s);
                char *old = t->heads[best];

                t->heads[best] = old ? g_strconcat(old, " ", w, NULL)
                    : g_strdup(w);
                g_free(old);
                g_free(w);
            }
        }
    }
    for (int k = 0; k < t->ncols; k++)
        if (!t->heads[k])
            t->heads[k] = g_strdup("");
    return t;
}

/* Sort numbers as numbers: "96%", "$600", "3.6K", and sectors by row */
static gboolean
cell_number(const char *s, double *v)
{
    char *end;
    double d;

    if (!s)
        return FALSE;
    while (*s == ' ' || *s == '$' || *s == '+')
        s++;
    if (!*s)
        return FALSE;
    d = g_ascii_strtod(s, &end);
    if (end == s)
        return FALSE;
    if (*end == ',') {          /* a sector: by y, then x */
        double y = g_ascii_strtod(end + 1, &end);

        d = y * 100000 + d;
    } else if (*end == 'K') {
        d *= 1000;
        end++;
    } else if (*end == 'M') {
        d *= 1000000;
        end++;
    }
    if (*end == '%')
        end++;
    *v = d;
    return !*end;
}

static int
compare_cells(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer col)
{
    char *sa, *sb;
    double va, vb;
    int r;

    gtk_tree_model_get(m, a, GPOINTER_TO_INT(col), &sa, -1);
    gtk_tree_model_get(m, b, GPOINTER_TO_INT(col), &sb, -1);
    if (cell_number(sa, &va) && cell_number(sb, &vb))
        r = va < vb ? -1 : va > vb;
    else
        r = g_strcmp0(sa, sb);
    g_free(sa);
    g_free(sb);
    return r;
}

static char *
cell_text(const char *line, int s, int e)
{
    int n = strlen(line);

    if (s >= n)
        return g_strdup("");
    return g_strstrip(g_strndup(line + s, MIN(e, n) - s));
}

/* Find the sector a row is about: the first cell that is "x,y" */
static gboolean
row_sector(GtkTreeModel *m, GtkTreeIter *it, int ncols, int *x, int *y)
{
    for (int k = 0; k < ncols; k++) {
        char *s, *end;
        gboolean ok = FALSE;

        gtk_tree_model_get(m, it, k, &s, -1);
        if (s) {
            long a = strtol(s, &end, 10);

            if (end != s && *end == ',') {
                const char *q = end + 1;
                long b = strtol(q, &end, 10);

                if (end != q && !*end) {
                    *x = a;
                    *y = b;
                    ok = TRUE;
                }
            }
        }
        g_free(s);
        if (ok)
            return TRUE;
    }
    return FALSE;
}

static void
row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *c,
              gpointer data)
{
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    GtkTreeIter it;
    int x, y;

    if (gtk_tree_model_get_iter(m, &it, path)
        && row_sector(m, &it, gtk_tree_model_get_n_columns(m), &x, &y)) {
        mapview_center(x, y);
        mapview_select(x, y);
        gtk_window_present(mainwin_window());
    }
}

/* Build (or rebuild) the table view for a report */
static gboolean
fill_table(Report *r)
{
    Table *t = kinds[r->kind].table ? find_table(r->lines) : NULL;
    GtkListStore *store;
    GType *types;
    GList *cols;

    cols = gtk_tree_view_get_columns(GTK_TREE_VIEW(r->tree));
    for (GList *l = cols; l; l = l->next)
        gtk_tree_view_remove_column(GTK_TREE_VIEW(r->tree), l->data);
    g_list_free(cols);
    gtk_tree_view_set_model(GTK_TREE_VIEW(r->tree), NULL);
    if (!t)
        return FALSE;
    types = g_new(GType, t->ncols);
    for (int k = 0; k < t->ncols; k++)
        types[k] = G_TYPE_STRING;
    store = gtk_list_store_newv(t->ncols, types);
    g_free(types);
    for (int i = t->first; i <= t->last; i++) {
        const char *l = g_ptr_array_index(r->lines, i);
        GtkTreeIter it;

        gtk_list_store_append(store, &it);
        for (int k = 0; k < t->ncols; k++) {
            char *c = cell_text(l, t->start[k], t->end[k]);

            gtk_list_store_set(store, &it, k, c, -1);
            g_free(c);
        }
    }
    for (int k = 0; k < t->ncols; k++) {
        GtkCellRenderer *rd = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col;
        double dummy;
        GtkTreeIter it;
        char *first = NULL;

        g_object_set(rd, "family", "Monospace", NULL);
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &it))
            gtk_tree_model_get(GTK_TREE_MODEL(store), &it, k, &first, -1);
        if (cell_number(first, &dummy))
            g_object_set(rd, "xalign", 1.0, NULL);
        g_free(first);
        col = gtk_tree_view_column_new_with_attributes(t->heads[k], rd, "text",
                                                       k, NULL);
        gtk_tree_view_column_set_sort_column_id(col, k);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(store), k,
                                        compare_cells, GINT_TO_POINTER(k), NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(r->tree), col);
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(r->tree), GTK_TREE_MODEL(store));
    g_object_unref(store);
    table_free(t);
    return TRUE;
}

/* ------------------------------------------------------------ window */

static Report *
current_report(void)
{
    int page;
    GtkWidget *w;

    if (!rp.win)
        return NULL;
    page = gtk_notebook_get_current_page(GTK_NOTEBOOK(rp.notebook));
    if (page < 0)
        return NULL;
    w = gtk_notebook_get_nth_page(GTK_NOTEBOOK(rp.notebook), page);
    return g_object_get_data(G_OBJECT(w), "report");
}

static void
show_mode(Report *r)
{
    gboolean table = r->has_table
        && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rp.table_btn));

    gtk_stack_set_visible_child_name(GTK_STACK(r->stack),
                                     table ? "table" : "text");
}

static void
update_toolbar(void)
{
    Report *r = current_report();
    char *t;

    gtk_widget_set_sensitive(rp.table_btn, r && r->has_table);
    if (!r) {
        gtk_label_set_text(GTK_LABEL(rp.status), "");
        return;
    }
    show_mode(r);
    t = g_strdup_printf("%s%s", r->cmd, r->has_table
                        ? "    (double-click a row to find it on the map)" : "");
    gtk_label_set_text(GTK_LABEL(rp.status), t);
    g_free(t);
}

static void
report_free(gpointer p)
{
    Report *r = p;

    g_free(r->cmd);
    g_ptr_array_unref(r->lines);
    g_free(r);
}

static void
close_page(GtkButton *b, gpointer page)
{
    Report *r = g_object_get_data(G_OBJECT(page), "report");

    g_ptr_array_remove(rp.reports, r);
    gtk_widget_destroy(page);
    if (!gtk_notebook_get_n_pages(GTK_NOTEBOOK(rp.notebook)))
        gtk_widget_destroy(rp.win);
}

static Report *
new_page(int kind)
{
    Report *r = g_new0(Report, 1);
    GtkWidget *sw, *tab, *close;

    r->kind = kind;
    r->lines = g_ptr_array_new_with_free_func(g_free);
    r->stack = gtk_stack_new();
    r->text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(r->text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(r->text), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(r->text), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(r->text), 4);
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), r->text);
    gtk_stack_add_named(GTK_STACK(r->stack), sw, "text");
    r->tree = gtk_tree_view_new();
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(r->tree), FALSE);
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(r->tree),
                                 GTK_TREE_VIEW_GRID_LINES_VERTICAL);
    g_signal_connect(r->tree, "row-activated", G_CALLBACK(row_activated), NULL);
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), r->tree);
    gtk_stack_add_named(GTK_STACK(r->stack), sw, "table");
    r->page = r->stack;
    g_object_set_data(G_OBJECT(r->page), "report", r);

    tab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    r->label = gtk_label_new(kinds[kind].title);
    gtk_box_pack_start(GTK_BOX(tab), r->label, FALSE, FALSE, 0);
    close = gtk_button_new_from_icon_name("window-close-symbolic",
                                          GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(close, "Close this report");
    if (!gtk_button_get_image(GTK_BUTTON(close)))
        gtk_button_set_label(GTK_BUTTON(close), "x");
    g_signal_connect(close, "clicked", G_CALLBACK(close_page), r->page);
    gtk_box_pack_start(GTK_BOX(tab), close, FALSE, FALSE, 0);
    gtk_widget_show_all(tab);
    gtk_widget_show_all(r->page);
    gtk_notebook_append_page(GTK_NOTEBOOK(rp.notebook), r->page, tab);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(rp.notebook), r->page, TRUE);
    g_ptr_array_add(rp.reports, r);
    return r;
}

static void
on_refresh(GtkWidget *w, gpointer d)
{
    Report *r = current_report();

    if (r && conn_state() == CONN_PLAYING)
        conn_command(r->cmd, CMD_SHOW | CMD_USER, NULL, NULL, NULL);
}

static char *
report_text(Report *r)
{
    GString *s = g_string_new(NULL);

    for (guint i = 0; i < r->lines->len; i++) {
        g_string_append(s, g_ptr_array_index(r->lines, i));
        g_string_append_c(s, '\n');
    }
    return g_string_free(s, FALSE);
}

static void
on_copy(GtkWidget *w, gpointer d)
{
    Report *r = current_report();
    char *t;

    if (!r)
        return;
    t = report_text(r);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), t, -1);
    g_free(t);
    gtk_label_set_text(GTK_LABEL(rp.status), "Copied to the clipboard.");
}

static void
on_save(GtkWidget *w, gpointer d)
{
    Report *r = current_report();
    GtkWidget *dlg;
    char *name, *t;
    GError *err = NULL;

    if (!r)
        return;
    dlg = gtk_file_chooser_dialog_new("Save Report", GTK_WINDOW(rp.win),
                                      GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel",
                                      GTK_RESPONSE_CANCEL, "_Save",
                                      GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    name = g_strdup_printf("%s.txt", kinds[r->kind].cmd);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), name);
    g_free(name);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg),
                                        g_get_home_dir());
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));

        t = report_text(r);
        if (!g_file_set_contents(file, t, -1, &err)) {
            mainwin_error("Could not save the report", err->message);
            g_error_free(err);
        }
        g_free(t);
        g_free(file);
    }
    gtk_widget_destroy(dlg);
}

static void
table_toggled(GtkToggleButton *b, gpointer d)
{
    Report *r = current_report();

    if (r)
        show_mode(r);
}

static gboolean
update_toolbar_idle(gpointer d)
{
    if (rp.win)
        update_toolbar();
    return G_SOURCE_REMOVE;
}

static void
page_switched(GtkNotebook *nb, GtkWidget *page, guint n, gpointer d)
{
    /* the current page is updated after this signal */
    g_idle_add(update_toolbar_idle, NULL);
}

static void
destroyed(GtkWidget *w, gpointer d)
{
    g_ptr_array_unref(rp.reports);
    rp.reports = NULL;
    rp.win = rp.notebook = rp.table_btn = rp.status = NULL;
}

static gboolean
key_press(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    if (ev->keyval == GDK_KEY_Escape) {
        gtk_widget_destroy(w);
        return TRUE;
    }
    return FALSE;
}

static GtkToolItem *
tool(GtkWidget *bar, const char *label, const char *tip, GCallback cb)
{
    GtkToolItem *b = gtk_tool_button_new(NULL, label);

    gtk_tool_button_set_use_underline(GTK_TOOL_BUTTON(b), TRUE);
    gtk_tool_item_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, NULL);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), b, -1);
    return b;
}

static void
make_window(void)
{
    GtkWidget *box, *bar;
    GtkToolItem *ti;

    rp.reports = g_ptr_array_new_with_free_func(report_free);
    rp.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(rp.win), "Reports");
    gtk_window_set_transient_for(GTK_WINDOW(rp.win), mainwin_window());
    gtk_window_set_default_size(GTK_WINDOW(rp.win), 820, 480);
    /* don't take the keyboard away from the command line */
    gtk_window_set_focus_on_map(GTK_WINDOW(rp.win), FALSE);
    g_signal_connect(rp.win, "destroy", G_CALLBACK(destroyed), NULL);
    g_signal_connect(rp.win, "key-press-event", G_CALLBACK(key_press), NULL);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(rp.win), box);
    bar = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(bar), GTK_TOOLBAR_TEXT);
    tool(bar, "_Refresh", "Run this report again", G_CALLBACK(on_refresh));
    tool(bar, "_Copy", "Copy the report text", G_CALLBACK(on_copy));
    tool(bar, "_Save...", "Save the report to a file", G_CALLBACK(on_save));
    gtk_toolbar_insert(GTK_TOOLBAR(bar), gtk_separator_tool_item_new(), -1);
    ti = gtk_tool_item_new();
    rp.table_btn = gtk_check_button_new_with_mnemonic("Show as _table");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rp.table_btn), TRUE);
    g_signal_connect(rp.table_btn, "toggled", G_CALLBACK(table_toggled), NULL);
    gtk_container_add(GTK_CONTAINER(ti), rp.table_btn);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), ti, -1);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    rp.notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(rp.notebook), TRUE);
    g_signal_connect(rp.notebook, "switch-page", G_CALLBACK(page_switched),
                     NULL);
    gtk_box_pack_start(GTK_BOX(box), rp.notebook, TRUE, TRUE, 0);
    rp.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(rp.status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(rp.status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(rp.status, 6);
    gtk_widget_set_margin_end(rp.status, 6);
    gtk_widget_set_margin_top(rp.status, 2);
    gtk_widget_set_margin_bottom(rp.status, 2);
    gtk_box_pack_start(GTK_BOX(box), rp.status, FALSE, FALSE, 0);
    gtk_widget_show_all(rp.win);
}

static void
show_report(int kind, const char *cmd, GPtrArray *lines)
{
    Report *r = NULL;
    GtkTextBuffer *buf;
    char *t;

    if (!rp.win)
        make_window();
    for (guint i = 0; i < rp.reports->len; i++)
        if (((Report *)g_ptr_array_index(rp.reports, i))->kind == kind)
            r = g_ptr_array_index(rp.reports, i);
    if (!r)
        r = new_page(kind);
    g_free(r->cmd);
    r->cmd = g_strdup(cmd);
    g_ptr_array_set_size(r->lines, 0);
    for (guint i = 0; i < lines->len; i++)
        g_ptr_array_add(r->lines, g_strdup(g_ptr_array_index(lines, i)));
    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(r->text));
    t = report_text(r);
    gtk_text_buffer_set_text(buf, t, -1);
    g_free(t);
    r->has_table = fill_table(r);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(rp.notebook),
                                  gtk_notebook_page_num(GTK_NOTEBOOK(rp.notebook),
                                                        r->page));
    update_toolbar();
    gtk_widget_show(rp.win);
}

void
reports_show_window(void)
{
    if (rp.win)
        gtk_window_present(GTK_WINDOW(rp.win));
    else
        mainwin_error("No reports yet",
                      "Run a report (from the Reports menu, the toolbar or the "
                      "command line) and it will appear in the Reports window.");
}

/* ----------------------------------------------------------- capture */

void
report_command_start(const char *cmd)
{
    rp.capturing = reports_enabled() ? report_kind(cmd) : -1;
    if (rp.capturing < 0)
        return;
    g_free(rp.cmd);
    rp.cmd = g_strstrip(g_strdup(cmd));
    if (!rp.lines)
        rp.lines = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_set_size(rp.lines, 0);
}

void
report_line(const char *text)
{
    if (rp.capturing >= 0)
        g_ptr_array_add(rp.lines, g_strdup(text));
}

void
report_command_end(void)
{
    int kind = rp.capturing;

    rp.capturing = -1;
    if (kind < 0 || !rp.lines || !rp.lines->len || conn_state() == CONN_DOWN)
        return;
    /* drop blank lines at either end */
    while (rp.lines->len && !*(char *)g_ptr_array_index(rp.lines, 0))
        g_ptr_array_remove_index(rp.lines, 0);
    while (rp.lines->len
           && !*(char *)g_ptr_array_index(rp.lines, rp.lines->len - 1))
        g_ptr_array_remove_index(rp.lines, rp.lines->len - 1);
    if (rp.lines->len)
        show_report(kind, rp.cmd, rp.lines);
}
