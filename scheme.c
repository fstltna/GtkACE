/*
 * scheme.c - map colour schemes (WinACE frmColorScheme and the
 * COLORSCHEME_* modes of HexGrid).
 *
 *   Normal        your land, sea, unseen and other land, in colours of
 *                 your choice; other countries in their player colours
 *   Conditional   sectors matching a condition such as "des=k&civ#0"
 *                 in one colour, the rest in another
 *   Graduated     sectors shaded by a value (food, eff, ...) between a
 *                 low and a high colour, optionally showing the value
 *   Distribution  sectors with a threshold for a commodity, and lines
 *                 to their distribution sectors
 *
 * Conditions use Empire's own syntax: field, operator (= # < > <= >=),
 * value; joined with & (and) or | (or).  Field names are the server's
 * (civil, milit, effic ...) or the short forms Empire accepts (civ,
 * mil, eff, mob, sh, pet ...).
 */
#include <string.h>
#include <stdlib.h>
#include <sqlite3.h>
#include "gtkace.h"

ColourScheme scheme;
static GArray *dist_lines;

/* -------------------------------------------------------- defaults */

static void
rgba(GdkRGBA *c, int r, int g, int b)
{
    c->red = r / 255.0;
    c->green = g / 255.0;
    c->blue = b / 255.0;
    c->alpha = 1;
}

static void
default_colours(void)
{
    /* WinACE's SetDefaultBasicColors, ComparisionColors, GradColors */
    rgba(&scheme.land, 0, 255, 135);
    rgba(&scheme.sea, 0, 150, 255);
    rgba(&scheme.unknown, 0, 0, 0);
    rgba(&scheme.other, 188, 188, 188);
    rgba(&scheme.met, 255, 0, 0);
    rgba(&scheme.missed, 0, 255, 135);
    rgba(&scheme.low_col, 255, 255, 0);
    rgba(&scheme.high_col, 255, 0, 0);
    rgba(&scheme.route, 255, 140, 0);
    rgba(&scheme.noroute, 0, 255, 135);
}

void
scheme_defaults(void)
{
    g_free(scheme.condition);
    g_free(scheme.field);
    memset(&scheme, 0, sizeof(scheme));
    scheme.mode = SCHEME_NORMAL;
    scheme.condition = g_strdup("");
    scheme.field = g_strdup("effic");
    scheme.auto_range = TRUE;
    scheme.low = 0;
    scheme.high = 100;
    scheme.dist_item = 'f';
    default_colours();
}

/* ----------------------------------------------------- persistence */

static const struct { const char *key; size_t off; } colour_keys[] = {
    { "land", G_STRUCT_OFFSET(ColourScheme, land) },
    { "sea", G_STRUCT_OFFSET(ColourScheme, sea) },
    { "unknown", G_STRUCT_OFFSET(ColourScheme, unknown) },
    { "other", G_STRUCT_OFFSET(ColourScheme, other) },
    { "met", G_STRUCT_OFFSET(ColourScheme, met) },
    { "missed", G_STRUCT_OFFSET(ColourScheme, missed) },
    { "low", G_STRUCT_OFFSET(ColourScheme, low_col) },
    { "high", G_STRUCT_OFFSET(ColourScheme, high_col) },
    { "route", G_STRUCT_OFFSET(ColourScheme, route) },
    { "noroute", G_STRUCT_OFFSET(ColourScheme, noroute) },
};

static char *
get(const char *key)
{
    char *k = g_strconcat("scheme.", key, NULL), *v = db_get_setting(k);

    g_free(k);
    return v;
}

static void
put(const char *key, const char *value)
{
    char *k = g_strconcat("scheme.", key, NULL);

    db_set_setting(k, value);
    g_free(k);
}

static void
put_int(const char *key, int v)
{
    char buf[32];

    snprintf(buf, sizeof(buf), "%d", v);
    put(key, buf);
}

static int
get_int(const char *key, int dflt)
{
    char *v = get(key);
    int r = v ? atoi(v) : dflt;

    g_free(v);
    return r;
}

void
scheme_load(void)
{
    char *v;

    scheme_defaults();
    if (!db_is_open())
        return;
    scheme.mode = CLAMP(get_int("mode", SCHEME_NORMAL), 0, SCHEME_COUNT - 1);
    if ((v = get("condition"))) {
        g_free(scheme.condition);
        scheme.condition = v;
    }
    if ((v = get("field"))) {
        g_free(scheme.field);
        scheme.field = v;
    }
    scheme.include_enemy = get_int("include_enemy", 0);
    scheme.auto_range = get_int("auto_range", 1);
    scheme.low = get_int("low", 0);
    scheme.high = get_int("high", 100);
    scheme.show_value = get_int("show_value", 0);
    if ((v = get("dist_item"))) {
        if (*v)
            scheme.dist_item = *v;
        g_free(v);
    }
    for (guint i = 0; i < G_N_ELEMENTS(colour_keys); i++) {
        char *k = g_strconcat("colour.", colour_keys[i].key, NULL);

        v = get(k);
        if (v)
            gdk_rgba_parse(G_STRUCT_MEMBER_P(&scheme, colour_keys[i].off), v);
        g_free(v);
        g_free(k);
    }
}

void
scheme_save(void)
{
    char item[2] = { scheme.dist_item, 0 };

    if (!db_is_open())
        return;
    put_int("mode", scheme.mode);
    put("condition", scheme.condition);
    put("field", scheme.field);
    put_int("include_enemy", scheme.include_enemy);
    put_int("auto_range", scheme.auto_range);
    put_int("low", scheme.low);
    put_int("high", scheme.high);
    put_int("show_value", scheme.show_value);
    put("dist_item", item);
    for (guint i = 0; i < G_N_ELEMENTS(colour_keys); i++) {
        char *k = g_strconcat("colour.", colour_keys[i].key, NULL);
        char *v = gdk_rgba_to_string(G_STRUCT_MEMBER_P(&scheme,
                                                       colour_keys[i].off));

        put(k, v);
        g_free(v);
        g_free(k);
    }
}

/* ------------------------------------------------------ conditions */

/* Short names Empire accepts, and the columns they mean */
static const char *aliases[][2] = {
    { "civ", "civil" }, { "mil", "milit" }, { "sh", "shell" },
    { "shells", "shell" }, { "guns", "gun" }, { "pet", "petrol" },
    { "bars", "bar" }, { "eff", "effic" }, { "mob", "mobil" },
    { "own", "owner" }, { "def", "dfense" }, { "defense", "dfense" },
    { "terr", "terr0" }, { "ocon", "ocontent" }, { "x", "xloc" },
    { "y", "yloc" }, { "fall", "fallout" }, { "uws", "uw" },
};

/* Column names in the intelligence table, where they differ */
static const char *intel_names[][2] = {
    { "effic", "eff" }, { "civil", "civ" }, { "milit", "mil" },
    { "dfense", "defense" }, { "xloc", "x" }, { "yloc", "y" },
};

static const char *
canonical(const char *name)
{
    for (guint i = 0; i < G_N_ELEMENTS(aliases); i++)
        if (!g_ascii_strcasecmp(name, aliases[i][0]))
            return aliases[i][1];
    return name;
}

enum { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

typedef struct {
    char *col;                  /* column, or "des"/"newdes" */
    int op;
    int num;                    /* numeric value */
    char chr;                   /* designation value */
} Clause;

typedef struct {
    GPtrArray *groups;          /* of GPtrArray of Clause*: OR of ANDs */
} Cond;

static void
clause_free(gpointer p)
{
    Clause *c = p;

    g_free(c->col);
    g_free(c);
}

static void
cond_free(Cond *c)
{
    if (c) {
        g_ptr_array_unref(c->groups);
        g_free(c);
    }
}

static gboolean
is_des_field(const char *col)
{
    return !strcmp(col, "des") || !strcmp(col, "newdes");
}

/* Parse "des=k&civ#0|mob>100".  Columns are checked against the
 * sector table when cols is given. */
static Cond *
cond_parse(const char *text, GHashTable *cols, char **error)
{
    Cond *c = g_new0(Cond, 1);
    char **ors;

    c->groups = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
    /* WinACE wrote && and ||; Empire writes & */
    {
        char *t = g_strdup(text), *p, *q;

        for (p = q = t; *p; p++) {
            if ((*p == '&' || *p == '|') && p[1] == *p)
                p++;
            if (*p != ' ')
                *q++ = *p;
        }
        *q = 0;
        ors = g_strsplit(t, "|", -1);
        g_free(t);
    }
    for (int i = 0; ors[i]; i++) {
        GPtrArray *ands = g_ptr_array_new_with_free_func(clause_free);
        char **parts = g_strsplit(ors[i], "&", -1);

        g_ptr_array_add(c->groups, ands);
        for (int j = 0; parts[j]; j++) {
            const char *s = parts[j], *o = s + strcspn(s, "=#<>!");
            Clause *cl;
            char *name, *end;
            const char *val;

            if (!*s || !*o || o == s) {
                *error = g_strdup_printf("\"%s\" is not field, operator, "
                                         "value (like civ>100)", s);
                goto fail;
            }
            cl = g_new0(Clause, 1);
            g_ptr_array_add(ands, cl);
            name = g_ascii_strdown(s, o - s);
            cl->col = g_strdup(canonical(name));
            g_free(name);
            if (o[0] == '<' && o[1] == '=') cl->op = OP_LE, val = o + 2;
            else if (o[0] == '>' && o[1] == '=') cl->op = OP_GE, val = o + 2;
            else if (o[0] == '!' && o[1] == '=') cl->op = OP_NE, val = o + 2;
            else if (o[0] == '<' && o[1] == '>') cl->op = OP_NE, val = o + 2;
            else if (o[0] == '=') cl->op = OP_EQ, val = o + 1;
            else if (o[0] == '#') cl->op = OP_NE, val = o + 1;
            else if (o[0] == '<') cl->op = OP_LT, val = o + 1;
            else if (o[0] == '>') cl->op = OP_GT, val = o + 1;
            else {
                *error = g_strdup_printf("unknown operator in \"%s\"", s);
                goto fail;
            }
            if (!*val) {
                *error = g_strdup_printf("\"%s\" has no value", s);
                goto fail;
            }
            if (is_des_field(cl->col)) {
                if (val[1] || (cl->op != OP_EQ && cl->op != OP_NE)) {
                    *error = g_strdup_printf("%s takes one letter with = or "
                                             "# (like des=k)", cl->col);
                    goto fail;
                }
                cl->chr = val[0];
            } else {
                cl->num = strtol(val, &end, 10);
                if (*end) {
                    *error = g_strdup_printf("\"%s\" is not a number", val);
                    goto fail;
                }
                if (cols && !g_hash_table_contains(cols, cl->col)) {
                    *error = g_strdup_printf("there is no field \"%s\"",
                                             cl->col);
                    goto fail;
                }
            }
        }
        g_strfreev(parts);
        continue;
fail:
        g_strfreev(parts);
        g_strfreev(ors);
        cond_free(c);
        return NULL;
    }
    g_strfreev(ors);
    return c;
}

static gboolean
compare(int a, int op, int b)
{
    switch (op) {
    case OP_EQ: return a == b;
    case OP_NE: return a != b;
    case OP_LT: return a < b;
    case OP_GT: return a > b;
    case OP_LE: return a <= b;
    default: return a >= b;
    }
}

/* Value of a column in the current row; FALSE if the row hasn't it */
typedef struct {
    sqlite3_stmt *st;
    GHashTable *idx;            /* column name -> index + 1 */
    gboolean intel;
} Row;

static gboolean
row_value(Row *r, const char *col, int *v)
{
    int i = GPOINTER_TO_INT(g_hash_table_lookup(r->idx, col));

    if (!i && r->intel)
        for (guint k = 0; k < G_N_ELEMENTS(intel_names); k++)
            if (!strcmp(col, intel_names[k][0]))
                i = GPOINTER_TO_INT(g_hash_table_lookup(r->idx,
                                                        intel_names[k][1]));
    if (!i || sqlite3_column_type(r->st, i - 1) == SQLITE_NULL)
        return FALSE;
    *v = sqlite3_column_int(r->st, i - 1);
    return TRUE;
}

static gboolean
cond_eval(Cond *c, Row *r, const Sector *s)
{
    for (guint i = 0; i < c->groups->len; i++) {
        GPtrArray *ands = g_ptr_array_index(c->groups, i);
        gboolean all = ands->len > 0;

        for (guint j = 0; j < ands->len && all; j++) {
            Clause *cl = g_ptr_array_index(ands, j);
            int v;

            if (is_des_field(cl->col)) {
                char d = !strcmp(cl->col, "des") ? s->des
                    : s->newdes ? s->newdes : s->des;

                all = cl->op == OP_EQ ? d == cl->chr : d != cl->chr;
            } else {
                all = row_value(r, cl->col, &v) && compare(v, cl->op, cl->num);
            }
        }
        if (all)
            return TRUE;
    }
    return FALSE;
}

static GHashTable *
column_index(sqlite3_stmt *st)
{
    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    int n = sqlite3_column_count(st);

    for (int i = 0; i < n; i++)
        g_hash_table_insert(h, g_strdup(sqlite3_column_name(st, i)),
                            GINT_TO_POINTER(i + 1));
    return h;
}

/* The sector table's columns, to check field names */
static GHashTable *
sect_columns(void)
{
    sqlite3_stmt *st;
    GHashTable *h = NULL;

    if (db_is_open() && db_table_exists("sect")
        && sqlite3_prepare_v2(db_handle(), "SELECT * FROM sect LIMIT 0", -1, &st,
                              NULL) == SQLITE_OK) {
        h = column_index(st);
        sqlite3_finalize(st);
    }
    return h;
}

gboolean
scheme_check_condition(const char *cond, char **error)
{
    GHashTable *cols = sect_columns();
    Cond *c;

    *error = NULL;
    if (!*cond) {
        *error = g_strdup("enter a condition");
        if (cols)
            g_hash_table_destroy(cols);
        return FALSE;
    }
    c = cond_parse(cond, cols, error);
    if (cols)
        g_hash_table_destroy(cols);
    cond_free(c);
    return c != NULL;
}

/* ------------------------------------------------------- computing */

/* Visit our sectors, and if asked the sectors seen of others */
static void
each_row(gboolean intel_too, void (*fn)(Row *, Sector *, int, int, gpointer),
         gpointer data)
{
    static const char *sql[2] = { "SELECT * FROM sect", "SELECT * FROM intel_sect" };
    static const char *tables[2] = { "sect", "intel_sect" };

    for (int t = 0; t < (intel_too ? 2 : 1); t++) {
        Row r = { NULL, NULL, t == 1 };
        int ix, iy;

        if (!db_table_exists(tables[t])
            || sqlite3_prepare_v2(db_handle(), sql[t], -1, &r.st, NULL)
            != SQLITE_OK)
            continue;
        r.idx = column_index(r.st);
        ix = GPOINTER_TO_INT(g_hash_table_lookup(r.idx, t ? "x" : "xloc")) - 1;
        iy = GPOINTER_TO_INT(g_hash_table_lookup(r.idx, t ? "y" : "yloc")) - 1;
        while (ix >= 0 && iy >= 0 && sqlite3_step(r.st) == SQLITE_ROW) {
            int x = sqlite3_column_int(r.st, ix), y = sqlite3_column_int(r.st, iy);
            Sector *s = world_sector(x, y);

            /* sightings of what is now ours don't count */
            if (!s || (t == 1 && s->own >= 0 && s->own == my_cnum))
                continue;
            fn(&r, s, x, y, data);
        }
        sqlite3_finalize(r.st);
        g_hash_table_destroy(r.idx);
    }
}

static void
do_condition(Row *r, Sector *s, int x, int y, gpointer data)
{
    s->smark = SM_HAS | (cond_eval(data, r, s) ? SM_MET : 0);
}

typedef struct {
    const char *col;
    int lo, hi;
    gboolean any;
} Grad;

static void
do_graduated(Row *r, Sector *s, int x, int y, gpointer data)
{
    Grad *g = data;
    int v;

    if (!row_value(r, g->col, &v))
        return;
    s->smark = SM_HAS;
    s->sval = v;
    if (!g->any || v < g->lo)
        g->lo = v;
    if (!g->any || v > g->hi)
        g->hi = v;
    g->any = TRUE;
}

static void
do_distribution(Row *r, Sector *s, int x, int y, gpointer data)
{
    char col[8];
    int thresh = 0, dx = x, dy = y;

    snprintf(col, sizeof(col), "%c_dist", scheme.dist_item);
    row_value(r, col, &thresh);
    row_value(r, "xdist", &dx);
    row_value(r, "ydist", &dy);
    s->smark = SM_HAS | (thresh > 0 ? SM_ROUTE : 0);
    s->sval = thresh;
    if (thresh > 0 && (dx != x || dy != y)) {
        int line[4] = { x, y, dx, dy };

        g_array_append_vals(dist_lines, line, 4);
    }
}

void
scheme_compute(void)
{
    if (!dist_lines)
        dist_lines = g_array_new(FALSE, FALSE, sizeof(int));
    g_array_set_size(dist_lines, 0);
    if (world_x <= 0)
        return;
    for (int y = -world_y / 2; y < world_y / 2; y++)
        for (int x = -world_x / 2; x < world_x / 2; x++) {
            Sector *s = world_sector(x, y);

            if (s) {
                s->smark = 0;
                s->sval = 0;
            }
        }
    switch (scheme.mode) {
    case SCHEME_CONDITIONAL: {
        char *err = NULL;
        Cond *c = *scheme.condition
            ? cond_parse(scheme.condition, NULL, &err) : NULL;

        g_free(err);
        if (c) {
            each_row(scheme.include_enemy, do_condition, c);
            cond_free(c);
        }
        break;
    }
    case SCHEME_GRADUATED: {
        Grad g = { canonical(scheme.field), 0, 0, FALSE };

        each_row(scheme.include_enemy, do_graduated, &g);
        if (scheme.auto_range && g.any) {
            scheme.low = g.lo;
            scheme.high = g.hi;
        }
        break;
    }
    case SCHEME_DISTRIBUTION:
        each_row(FALSE, do_distribution, NULL);
        break;
    default:
        break;
    }
}

GArray *
scheme_dist_lines(void)
{
    return scheme.mode == SCHEME_DISTRIBUTION ? dist_lines : NULL;
}

static const char *
item_label(char letter)
{
    static const char *names[][2] = {
        { "c", "civilians" }, { "m", "military" }, { "u", "uw" },
        { "f", "food" }, { "s", "shells" }, { "g", "guns" },
        { "p", "petrol" }, { "i", "iron" }, { "d", "dust" },
        { "b", "bars" }, { "o", "oil" }, { "l", "lcm" }, { "h", "hcm" },
        { "r", "rads" },
    };

    for (guint i = 0; i < G_N_ELEMENTS(names); i++)
        if (names[i][0][0] == letter)
            return names[i][1];
    return "?";
}

char *
scheme_legend(void)
{
    switch (scheme.mode) {
    case SCHEME_CONDITIONAL:
        return g_strdup_printf("%s", *scheme.condition ? scheme.condition
                               : "(no condition)");
    case SCHEME_GRADUATED:
        return g_strdup_printf("%s %d .. %d", scheme.field, scheme.low,
                               scheme.high);
    case SCHEME_DISTRIBUTION:
        return g_strdup_printf("%s thresholds", item_label(scheme.dist_item));
    default:
        return NULL;
    }
}

/* =========================================================== dialog */

static struct {
    GtkWidget *win, *mode[SCHEME_COUNT], *stack;
    GtkWidget *cond, *cond_err, *cond_enemy;
    GtkWidget *field, *auto_range, *low, *high, *show_value, *grad_enemy;
    GtkWidget *dist_item;
    GHashTable *buttons;        /* colour key -> GtkColorButton */
} sd;

static const char *mode_names[SCHEME_COUNT] = {
    "Normal", "Conditional", "Graduated", "Distribution"
};

static GtkWidget *
colour_button(const char *key, const char *label, GtkWidget *grid, int row)
{
    GtkWidget *b = gtk_color_button_new(), *l = gtk_label_new(label);

    for (guint i = 0; i < G_N_ELEMENTS(colour_keys); i++)
        if (!strcmp(colour_keys[i].key, key))
            gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(b),
                                       G_STRUCT_MEMBER_P(&scheme,
                                                         colour_keys[i].off));
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_grid_attach(GTK_GRID(grid), b, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), l, 1, row, 1, 1);
    g_hash_table_insert(sd.buttons, (gpointer)key, b);
    return b;
}

static GtkWidget *
page_grid(void)
{
    GtkWidget *g = gtk_grid_new();

    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g), 8);
    gtk_container_set_border_width(GTK_CONTAINER(g), 6);
    return g;
}

static GtkWidget *
note(const char *text)
{
    GtkWidget *l = gtk_label_new(text);

    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 52);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
    return l;
}

static void
check_condition(void)
{
    char *err = NULL;
    const char *c = gtk_entry_get_text(GTK_ENTRY(sd.cond));

    if (!*c || scheme_check_condition(c, &err))
        gtk_label_set_text(GTK_LABEL(sd.cond_err), "");
    else {
        char *t = g_strdup_printf("Not understood: %s", err);

        gtk_label_set_text(GTK_LABEL(sd.cond_err), t);
        g_free(t);
    }
    g_free(err);
}

static void
range_toggled(void)
{
    gboolean a = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sd.auto_range));

    gtk_widget_set_sensitive(sd.low, !a);
    gtk_widget_set_sensitive(sd.high, !a);
}

static int
current_mode(void)
{
    for (int i = 0; i < SCHEME_COUNT; i++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sd.mode[i])))
            return i;
    return SCHEME_NORMAL;
}

static void
mode_toggled(GtkToggleButton *b, gpointer d)
{
    if (gtk_toggle_button_get_active(b))
        gtk_stack_set_visible_child_name(GTK_STACK(sd.stack),
                                         mode_names[GPOINTER_TO_INT(d)]);
}

/* Take the dialog's settings and redraw */
static void
apply(void)
{
    const char *f;

    scheme.mode = current_mode();
    g_free(scheme.condition);
    scheme.condition = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(sd.cond))));
    scheme.include_enemy = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(scheme.mode == SCHEME_GRADUATED ? sd.grad_enemy
                          : sd.cond_enemy));
    f = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(sd.field))));
    if (*f) {
        g_free(scheme.field);
        scheme.field = g_ascii_strdown(f, -1);
        g_strstrip(scheme.field);
    }
    scheme.auto_range = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(sd.auto_range));
    scheme.low = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sd.low));
    scheme.high = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sd.high));
    scheme.show_value = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(sd.show_value));
    {
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(sd.dist_item));

        if (id)
            scheme.dist_item = id[0];
    }
    for (guint i = 0; i < G_N_ELEMENTS(colour_keys); i++) {
        GtkWidget *b = g_hash_table_lookup(sd.buttons, colour_keys[i].key);

        if (b)
            gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(b),
                                       G_STRUCT_MEMBER_P(&scheme,
                                                         colour_keys[i].off));
    }
    scheme_save();
    scheme_compute();
    /* the computed range shows in the dialog */
    if (scheme.mode == SCHEME_GRADUATED && scheme.auto_range) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(sd.low), scheme.low);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(sd.high), scheme.high);
    }
    mapview_redraw();
}

static void
reset_colours(void)
{
    ColourScheme keep = scheme;
    GdkRGBA *c;

    default_colours();
    for (guint i = 0; i < G_N_ELEMENTS(colour_keys); i++) {
        GtkWidget *b = g_hash_table_lookup(sd.buttons, colour_keys[i].key);

        c = G_STRUCT_MEMBER_P(&scheme, colour_keys[i].off);
        if (b)
            gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(b), c);
    }
    /* only the buttons change until Apply */
    for (guint i = 0; i < G_N_ELEMENTS(colour_keys); i++)
        *(GdkRGBA *)G_STRUCT_MEMBER_P(&scheme, colour_keys[i].off)
            = *(GdkRGBA *)G_STRUCT_MEMBER_P(&keep, colour_keys[i].off);
}

static void
destroyed(GtkWidget *w, gpointer d)
{
    g_hash_table_destroy(sd.buttons);
    memset(&sd, 0, sizeof(sd));
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

static void
ok_clicked(void)
{
    apply();
    gtk_widget_destroy(sd.win);
}

void
scheme_dialog(void)
{
    static const char *fields[] = {
        "effic", "mobil", "civil", "milit", "uw", "food", "work", "avail",
        "shell", "gun", "petrol", "iron", "dust", "bar", "oil", "lcm", "hcm",
        "rad", "min", "gold", "fert", "ocontent", "uran", "road", "rail",
        "dfense", "fallout", NULL
    };
    static const char *items[] = { "c", "m", "u", "f", "s", "g", "p", "i",
                                   "d", "b", "o", "l", "h", "r", NULL };
    GtkWidget *box, *left, *g, *bb, *b;
    GSList *group = NULL;
    char buf[2] = { 0, 0 };

    if (sd.win) {
        gtk_window_present(GTK_WINDOW(sd.win));
        return;
    }
    sd.buttons = g_hash_table_new(g_str_hash, g_str_equal);
    sd.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(sd.win), "Colour Scheme");
    gtk_window_set_transient_for(GTK_WINDOW(sd.win), mainwin_window());
    gtk_window_set_type_hint(GTK_WINDOW(sd.win), GDK_WINDOW_TYPE_HINT_DIALOG);
    g_signal_connect(sd.win, "destroy", G_CALLBACK(destroyed), NULL);
    g_signal_connect(sd.win, "key-press-event", G_CALLBACK(key_press), NULL);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_container_add(GTK_CONTAINER(sd.win), box);

    g = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(box), g, TRUE, TRUE, 0);
    left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(g), left, FALSE, FALSE, 0);
    sd.stack = gtk_stack_new();
    gtk_box_pack_start(GTK_BOX(g), sd.stack, TRUE, TRUE, 0);

    for (int i = 0; i < SCHEME_COUNT; i++) {
        sd.mode[i] = gtk_radio_button_new_with_label(group, mode_names[i]);
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(sd.mode[i]));
        gtk_box_pack_start(GTK_BOX(left), sd.mode[i], FALSE, FALSE, 0);
        g_signal_connect(sd.mode[i], "toggled", G_CALLBACK(mode_toggled),
                         GINT_TO_POINTER(i));
    }

    /* Normal */
    g = page_grid();
    colour_button("land", "Your sectors", g, 0);
    colour_button("sea", "Sea", g, 1);
    colour_button("unknown", "Not seen", g, 2);
    colour_button("other", "Other land", g, 3);
    gtk_grid_attach(GTK_GRID(g), note("Other countries' sectors use their "
                                      "player colours."), 0, 4, 2, 1);
    gtk_stack_add_named(GTK_STACK(sd.stack), g, mode_names[0]);

    /* Conditional */
    g = page_grid();
    sd.cond = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(sd.cond), scheme.condition);
    gtk_entry_set_placeholder_text(GTK_ENTRY(sd.cond), "e.g. des=k&civ#0");
    gtk_widget_set_hexpand(sd.cond, TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(sd.cond), TRUE);
    gtk_grid_attach(GTK_GRID(g), gtk_label_new("Condition:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), sd.cond, 1, 0, 1, 1);
    sd.cond_err = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(sd.cond_err), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(sd.cond_err),
                                "error");
    gtk_grid_attach(GTK_GRID(g), sd.cond_err, 0, 1, 2, 1);
    gtk_grid_attach(GTK_GRID(g), note(
        "As in Empire: field, operator (= # < > <= >=), value; join with & "
        "(and) or | (or).\nExamples:  des=k&civ#0&iron>0    mob>0&mil>50    "
        "food<100|eff<60\nFields: des newdes eff mob civ mil uw food work avail "
        "sh gun pet iron dust bar oil lcm hcm rad min gold fert ocontent uran "
        "road rail def own ..."), 0, 2, 2, 1);
    b = page_grid();
    colour_button("met", "Condition met", b, 0);
    colour_button("missed", "Condition not met", b, 1);
    gtk_grid_attach(GTK_GRID(g), b, 0, 3, 2, 1);
    sd.cond_enemy = gtk_check_button_new_with_label(
        "Include other countries' sectors you have seen");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sd.cond_enemy),
                                 scheme.include_enemy);
    gtk_grid_attach(GTK_GRID(g), sd.cond_enemy, 0, 4, 2, 1);
    g_signal_connect_swapped(sd.cond, "changed", G_CALLBACK(check_condition),
                             NULL);
    gtk_stack_add_named(GTK_STACK(sd.stack), g, mode_names[1]);

    /* Graduated */
    g = page_grid();
    sd.field = gtk_combo_box_text_new_with_entry();
    for (int i = 0; fields[i]; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sd.field), fields[i]);
    gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(sd.field))),
                       scheme.field);
    gtk_grid_attach(GTK_GRID(g), gtk_label_new("Shade by:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), sd.field, 1, 0, 2, 1);
    sd.auto_range = gtk_check_button_new_with_label(
        "Range from the lowest to the highest value");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sd.auto_range),
                                 scheme.auto_range);
    gtk_grid_attach(GTK_GRID(g), sd.auto_range, 0, 1, 3, 1);
    sd.low = gtk_spin_button_new_with_range(-100000, 100000, 1);
    sd.high = gtk_spin_button_new_with_range(-100000, 100000, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sd.low), scheme.low);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sd.high), scheme.high);
    gtk_grid_attach(GTK_GRID(g), gtk_label_new("Low:"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(g), sd.low, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(g), gtk_label_new("High:"), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(g), sd.high, 1, 3, 1, 1);
    b = page_grid();
    colour_button("low", "Low colour", b, 0);
    colour_button("high", "High colour", b, 1);
    gtk_grid_attach(GTK_GRID(g), b, 0, 4, 3, 1);
    sd.show_value = gtk_check_button_new_with_label("Show the values on the map");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sd.show_value),
                                 scheme.show_value);
    gtk_grid_attach(GTK_GRID(g), sd.show_value, 0, 5, 3, 1);
    sd.grad_enemy = gtk_check_button_new_with_label(
        "Include other countries' sectors you have seen");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sd.grad_enemy),
                                 scheme.include_enemy);
    gtk_grid_attach(GTK_GRID(g), sd.grad_enemy, 0, 6, 3, 1);
    g_signal_connect_swapped(sd.auto_range, "toggled",
                             G_CALLBACK(range_toggled), NULL);
    gtk_stack_add_named(GTK_STACK(sd.stack), g, mode_names[2]);

    /* Distribution */
    g = page_grid();
    sd.dist_item = gtk_combo_box_text_new();
    for (int i = 0; items[i]; i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd.dist_item), items[i],
                                  item_label(items[i][0]));
    buf[0] = scheme.dist_item;
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(sd.dist_item), buf);
    gtk_grid_attach(GTK_GRID(g), gtk_label_new("Commodity:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), sd.dist_item, 1, 0, 1, 1);
    b = page_grid();
    colour_button("route", "Has a threshold", b, 0);
    colour_button("noroute", "No threshold", b, 1);
    gtk_grid_attach(GTK_GRID(g), b, 0, 1, 2, 1);
    gtk_grid_attach(GTK_GRID(g), note("Lines run from each sector with a "
                                      "threshold to its distribution sector."),
                    0, 2, 2, 1);
    gtk_stack_add_named(GTK_STACK(sd.stack), g, mode_names[3]);

    bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    b = gtk_button_new_with_mnemonic("_Reset Colours");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(reset_colours), NULL);
    gtk_box_pack_start(GTK_BOX(bb), b, FALSE, FALSE, 0);
    b = gtk_button_new_with_mnemonic("_OK");
    gtk_widget_set_can_default(b, TRUE);
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(ok_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(bb), b, FALSE, FALSE, 0);
    gtk_window_set_default(GTK_WINDOW(sd.win), b);
    b = gtk_button_new_with_mnemonic("_Apply");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(apply), NULL);
    gtk_box_pack_end(GTK_BOX(bb), b, FALSE, FALSE, 0);
    b = gtk_button_new_with_mnemonic("_Close");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_widget_destroy),
                             sd.win);
    gtk_box_pack_end(GTK_BOX(bb), b, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bb, FALSE, FALSE, 0);

    gtk_widget_show_all(sd.win);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sd.mode[scheme.mode]), TRUE);
    gtk_stack_set_visible_child_name(GTK_STACK(sd.stack),
                                     mode_names[scheme.mode]);
    range_toggled();
    check_condition();
}
