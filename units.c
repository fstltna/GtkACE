/*
 * units.c - lists of our ships, land units, planes and nukes
 * (WinACE frmUnits).  Double-click a unit to show it on the map.
 */
#include <string.h>
#include <sqlite3.h>
#include "gtkace.h"

typedef enum { K_INT, K_PCT, K_TYPE, K_XY, K_TEXT, K_CARRIER } Kind;

typedef struct {
    const char *col;            /* xdump column */
    const char *title;
    Kind kind;
} ColSpec;

#define END { NULL, NULL, 0 }

static const ColSpec ship_cols[] = {
    { "uid", "#", K_INT }, { "type", "Type", K_TYPE }, { "xloc", "x,y", K_XY },
    { "fleet", "Flt", K_TEXT }, { "effic", "Eff", K_PCT },
    { "mobil", "Mob", K_INT }, { "tech", "Tech", K_INT },
    { "civil", "Civ", K_INT }, { "milit", "Mil", K_INT },
    { "uw", "Uw", K_INT }, { "food", "Food", K_INT },
    { "shell", "Sh", K_INT }, { "gun", "Gun", K_INT },
    { "petrol", "Pet", K_INT }, { "oil", "Oil", K_INT },
    { "lcm", "Lcm", K_INT }, { "hcm", "Hcm", K_INT },
    { "name", "Name", K_TEXT }, END
};
static const ColSpec land_cols[] = {
    { "uid", "#", K_INT }, { "type", "Type", K_TYPE }, { "xloc", "x,y", K_XY },
    { "army", "Army", K_TEXT }, { "effic", "Eff", K_PCT },
    { "mobil", "Mob", K_INT }, { "tech", "Tech", K_INT },
    { "milit", "Mil", K_INT }, { "food", "Food", K_INT },
    { "shell", "Sh", K_INT }, { "gun", "Gun", K_INT },
    { "petrol", "Pet", K_INT }, { "retreat", "Retreat", K_PCT },
    { "ship", "On", K_CARRIER }, END
};
static const ColSpec plane_cols[] = {
    { "uid", "#", K_INT }, { "type", "Type", K_TYPE }, { "xloc", "x,y", K_XY },
    { "wing", "Wing", K_TEXT }, { "effic", "Eff", K_PCT },
    { "mobil", "Mob", K_INT }, { "tech", "Tech", K_INT },
    { "range", "Range", K_INT }, { "harden", "Hard", K_INT },
    { "ship", "On", K_CARRIER }, END
};
static const ColSpec nuke_cols[] = {
    { "uid", "#", K_INT }, { "type", "Type", K_TYPE }, { "xloc", "x,y", K_XY },
    { "effic", "Eff", K_PCT }, { "tech", "Tech", K_INT },
    { "plane", "On", K_CARRIER }, END
};
static const ColSpec *specs[UC_COUNT] = {
    ship_cols, land_cols, plane_cols, nuke_cols
};

#define MAX_COLS 24

typedef struct {
    GtkWidget *view;
    GtkListStore *store;
    int ncols;                  /* visible columns */
    int col_x, col_y;           /* hidden model columns with the location */
    const ColSpec *cols[MAX_COLS];
} Tab;

static struct {
    GtkWidget *win, *notebook, *count;
    Tab tabs[UC_COUNT];
} uw;

static int
col_index(sqlite3_stmt *st, const char *name)
{
    int n = sqlite3_column_count(st);

    for (int i = 0; i < n; i++)
        if (!strcmp(sqlite3_column_name(st, i), name))
            return i;
    return -1;
}

/* Text for the "On" column: what the unit is loaded on */
static char *
carrier_text(UnitClass c, sqlite3_stmt *st)
{
    int i;

    if (c == UC_NUKE) {
        i = col_index(st, "plane");
        if (i >= 0 && sqlite3_column_int(st, i) >= 0
            && sqlite3_column_type(st, i) != SQLITE_NULL)
            return g_strdup_printf("plane %d", sqlite3_column_int(st, i));
        return g_strdup("");
    }
    i = col_index(st, "ship");
    if (i >= 0 && sqlite3_column_type(st, i) != SQLITE_NULL
        && sqlite3_column_int(st, i) >= 0)
        return g_strdup_printf("ship %d", sqlite3_column_int(st, i));
    i = col_index(st, "land");
    if (i >= 0 && sqlite3_column_type(st, i) != SQLITE_NULL
        && sqlite3_column_int(st, i) >= 0)
        return g_strdup_printf("land %d", sqlite3_column_int(st, i));
    return g_strdup("");
}

static void
fill_tab(UnitClass c)
{
    Tab *t = &uw.tabs[c];
    sqlite3_stmt *st;
    char *sql;
    int idx[MAX_COLS], ix, iy, rows = 0;

    gtk_list_store_clear(t->store);
    if (!db_is_open() || !db_table_exists(world_unit_table(c)))
        return;
    sql = g_strdup_printf("SELECT * FROM \"%s\" ORDER BY uid",
                          world_unit_table(c));
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &st, NULL) != SQLITE_OK) {
        g_free(sql);
        return;
    }
    g_free(sql);
    for (int i = 0; i < t->ncols; i++)
        idx[i] = col_index(st, t->cols[i]->col);
    ix = col_index(st, "xloc");
    iy = col_index(st, "yloc");
    while (sqlite3_step(st) == SQLITE_ROW) {
        GtkTreeIter it;
        int x = ix >= 0 ? sqlite3_column_int(st, ix) : 0;
        int y = iy >= 0 ? sqlite3_column_int(st, iy) : 0;

        gtk_list_store_append(t->store, &it);
        gtk_list_store_set(t->store, &it, t->col_x, x, t->col_y, y, -1);
        for (int i = 0; i < t->ncols; i++) {
            const ColSpec *cs = t->cols[i];
            int v = idx[i] >= 0 ? sqlite3_column_int(st, idx[i]) : 0;
            const char *txt = idx[i] >= 0
                ? (const char *)sqlite3_column_text(st, idx[i]) : NULL;
            char *s = NULL;

            switch (cs->kind) {
            case K_INT:
            case K_PCT:
                gtk_list_store_set(t->store, &it, i, v, -1);
                continue;
            case K_TYPE:
                s = g_strdup_printf("%-4s %s", world_unit_mnem(c, v),
                                    world_unit_type_name(c, v));
                break;
            case K_XY:
                s = g_strdup_printf("%d,%d", x, y);
                break;
            case K_CARRIER:
                s = carrier_text(c, st);
                break;
            case K_TEXT:
                s = g_strdup(txt && strcmp(txt, "~") ? txt : "");
                break;
            }
            gtk_list_store_set(t->store, &it, i, s, -1);
            g_free(s);
        }
        rows++;
    }
    sqlite3_finalize(st);
}

static void
update_count(void)
{
    int c = gtk_notebook_get_current_page(GTK_NOTEBOOK(uw.notebook));
    int n;
    char *s;

    if (c < 0 || c >= UC_COUNT)
        return;
    n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(uw.tabs[c].store), NULL);
    s = g_strdup_printf("%d %s", n, world_unit_noun(c, n != 1));
    gtk_label_set_text(GTK_LABEL(uw.count), s);
    g_free(s);
}

void
units_window_refresh(void)
{
    if (!uw.win)
        return;
    for (int c = 0; c < UC_COUNT; c++)
        fill_tab(c);
    update_count();
}

static void
row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col,
              gpointer data)
{
    Tab *t = data;
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    GtkTreeIter it;
    int x, y;

    if (!gtk_tree_model_get_iter(m, &it, path))
        return;
    gtk_tree_model_get(m, &it, t->col_x, &x, t->col_y, &y, -1);
    mapview_center(x, y);
    mapview_select(x, y);
    gtk_window_present(mainwin_window());
}

/* XY column sorts by y, then x, like Empire's own lists */
static int
sort_xy(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer data)
{
    Tab *t = data;
    int ax, ay, bx, by;

    gtk_tree_model_get(m, a, t->col_x, &ax, t->col_y, &ay, -1);
    gtk_tree_model_get(m, b, t->col_x, &bx, t->col_y, &by, -1);
    return ay != by ? ay - by : ax - bx;
}

static void
pct_cell(GtkTreeViewColumn *col, GtkCellRenderer *r, GtkTreeModel *m,
         GtkTreeIter *it, gpointer data)
{
    int v;
    char buf[16];

    gtk_tree_model_get(m, it, GPOINTER_TO_INT(data), &v, -1);
    snprintf(buf, sizeof(buf), "%d%%", v);
    g_object_set(r, "text", buf, NULL);
}

static GtkWidget *
build_tab(UnitClass c)
{
    Tab *t = &uw.tabs[c];
    GType types[MAX_COLS + 2];
    GtkWidget *sw;
    int n = 0;

    for (const ColSpec *cs = specs[c]; cs->col && n < MAX_COLS; cs++) {
        t->cols[n] = cs;
        types[n] = cs->kind == K_INT || cs->kind == K_PCT
            ? G_TYPE_INT : G_TYPE_STRING;
        n++;
    }
    t->ncols = n;
    t->col_x = n;
    t->col_y = n + 1;
    types[n] = types[n + 1] = G_TYPE_INT;
    t->store = gtk_list_store_newv(n + 2, types);
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(t->store), t->col_x,
                                    sort_xy, t, NULL);
    t->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(t->store));
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(t->view), FALSE);
    for (int i = 0; i < n; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col;
        gboolean num = t->cols[i]->kind == K_INT || t->cols[i]->kind == K_PCT
            || t->cols[i]->kind == K_XY;

        g_object_set(r, "family", "Monospace", NULL);
        if (num)
            g_object_set(r, "xalign", 1.0, NULL);
        if (t->cols[i]->kind == K_PCT) {
            col = gtk_tree_view_column_new_with_attributes(t->cols[i]->title, r,
                                                           NULL);
            gtk_tree_view_column_set_cell_data_func(col, r, pct_cell,
                                                    GINT_TO_POINTER(i), NULL);
        } else {
            col = gtk_tree_view_column_new_with_attributes(t->cols[i]->title, r,
                                                           "text", i, NULL);
        }
        gtk_tree_view_column_set_sort_column_id(
            col, t->cols[i]->kind == K_XY ? t->col_x : i);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(t->view), col);
    }
    g_signal_connect(t->view, "row-activated", G_CALLBACK(row_activated), t);
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), t->view);
    return sw;
}

static void
refresh_clicked(GtkButton *b, gpointer d)
{
    refresh_units();
}

static void
page_switched(GtkNotebook *nb, GtkWidget *page, guint num, gpointer d)
{
    /* the page index is not current yet during the signal */
    int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(uw.tabs[num].store),
                                           NULL);
    char *s = g_strdup_printf("%d %s", n, world_unit_noun(num, n != 1));

    gtk_label_set_text(GTK_LABEL(uw.count), s);
    g_free(s);
}

static void
destroyed(GtkWidget *w, gpointer d)
{
    memset(&uw, 0, sizeof(uw));
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

void
units_window_show(UnitClass c)
{
    GtkWidget *box, *bar, *btn, *hint;

    if (!uw.win) {
        uw.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(uw.win), "Units");
        gtk_window_set_transient_for(GTK_WINDOW(uw.win), mainwin_window());
        gtk_window_set_default_size(GTK_WINDOW(uw.win), 760, 380);
        g_signal_connect(uw.win, "destroy", G_CALLBACK(destroyed), NULL);
        g_signal_connect(uw.win, "key-press-event", G_CALLBACK(key_press),
                         NULL);
        box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(box), 6);
        gtk_container_add(GTK_CONTAINER(uw.win), box);
        uw.notebook = gtk_notebook_new();
        for (int i = 0; i < UC_COUNT; i++) {
            char *label = g_strdup(world_unit_noun(i, TRUE));

            label[0] = g_ascii_toupper(label[0]);
            gtk_notebook_append_page(GTK_NOTEBOOK(uw.notebook), build_tab(i),
                                     gtk_label_new(label));
            g_free(label);
        }
        gtk_box_pack_start(GTK_BOX(box), uw.notebook, TRUE, TRUE, 0);
        bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        uw.count = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(bar), uw.count, FALSE, FALSE, 0);
        hint = gtk_label_new("Double-click a unit to find it on the map.");
        gtk_style_context_add_class(gtk_widget_get_style_context(hint),
                                    "dim-label");
        gtk_box_pack_start(GTK_BOX(bar), hint, TRUE, TRUE, 0);
        btn = gtk_button_new_with_mnemonic("_Update from Server");
        g_signal_connect(btn, "clicked", G_CALLBACK(refresh_clicked), NULL);
        gtk_box_pack_end(GTK_BOX(bar), btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
        g_signal_connect(uw.notebook, "switch-page", G_CALLBACK(page_switched),
                         NULL);
        units_window_refresh();
        gtk_widget_show_all(uw.win);
    }
    gtk_notebook_set_current_page(GTK_NOTEBOOK(uw.notebook), c);
    update_count();
    gtk_window_present(GTK_WINDOW(uw.win));
}
