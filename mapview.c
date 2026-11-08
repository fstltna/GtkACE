/*
 * mapview.c - the hex map (WinACE HexGrid/frmDrawMap picMap).
 *
 * Empire's world is a torus of hexes.  Sector (x, y) exists when x + y
 * is even; neighbours in a row are two x apart.  Hexes are drawn with
 * a point at the top, as WinACE does.
 */
#include <math.h>
#include <string.h>
#include "gtkace.h"
#include "icons.h"

gboolean show_units[UC_COUNT] = { TRUE, TRUE, TRUE, TRUE };

/* WinACE's unit icons.  Index: class (ship, land, plane) * 5 + kind,
 * kind 0 = ours, then REL_ENEMY + 1 ... REL_OLD + 1; 15 = nuke. */
static cairo_surface_t *
icon_surface(int i)
{
    static cairo_surface_t *surf[16];
    static const guint32 *data[16] = {
        icon_ship, icon_eship, icon_fship, icon_nship, icon_xship,
        icon_land, icon_eland, icon_fland, icon_nland, icon_xland,
        icon_plane, icon_eplane, icon_fplane, icon_nplane, icon_xplane,
        icon_nuke
    };

    if (!surf[i]) {
        int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, ICON_W);
        guchar *buf = g_malloc0(stride * ICON_H);

        for (int y = 0; y < ICON_H; y++)
            memcpy(buf + y * stride, data[i] + y * ICON_W, ICON_W * 4);
        surf[i] = cairo_image_surface_create_for_data(buf, CAIRO_FORMAT_ARGB32,
                                                      ICON_W, ICON_H, stride);
    }
    return surf[i];
}

/* icon index for unit class c (ship, land, plane) and kind */
static int
icon_index(int c, int kind)
{
    static const int row[UC_COUNT] = { 0, 5, 10, 15 };

    return c == UC_NUKE ? 15 : row[c] + kind;
}

/* Up to three icons per hex, placed as WinACE's DrawUnits does: our
 * units first, then enemy, allied, neutral and old sightings.
 * Returns how many were drawn. */
static int
draw_unit_icons(cairo_t *cr, const Sector *sec, double px, double py, double s)
{
    static const double pos[3][2] = {
        { -0.8, -0.5 }, { 0.0, -0.5 }, { -0.4, -0.8 }
    };
    double w = 0.8 * s, h = w * ICON_H / ICON_W;
    int icons[3], n = 0;

    for (int c = UC_SHIP; c <= UC_PLANE && n < 3; c++)
        if (sec->units[c] && show_units[c])
            icons[n++] = icon_index(c, 0);
    for (int k = REL_ENEMY; k <= REL_OLD && n < 3; k++)
        for (int c = UC_SHIP; c <= UC_PLANE && n < 3; c++)
            if (sec->foreign[c][k] && show_units[c])
                icons[n++] = icon_index(c, k + 1);
    if (n < 3 && sec->units[UC_NUKE] && show_units[UC_NUKE])
        icons[n++] = icon_index(UC_NUKE, 0);

    for (int i = 0; i < n; i++) {
        cairo_save(cr);
        cairo_translate(cr, px + pos[i][0] * s, py + pos[i][1] * s);
        cairo_scale(cr, w / ICON_W, h / ICON_H);
        cairo_set_source_surface(cr, icon_surface(icons[i]), 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    return n;
}

#define SQRT3_2 0.8660254
#define MIN_SIDE 4.0
#define MAX_SIDE 60.0
#define DRAG_SLOP 4

static struct {
    GtkWidget *area;
    double cx, cy;              /* world coordinates at the window centre */
    double side;                /* hex side length in pixels */
    gboolean have_sel;
    int sel_x, sel_y;
    /* dragging */
    gboolean pressed, dragging;
    double press_x, press_y, press_cx, press_cy;
    guint press_button;
    /* dialog that wants map clicks */
    MapPickFunc pick;
    gpointer pick_data;
    /* path being shown */
    char *path;
    int path_x, path_y;
} mv = { NULL, 0, 0, 16.0, FALSE, 0, 0 };

typedef struct { double r, g, b; } RGB;

/* The basic colours come from the colour scheme (scheme.c) */
static const RGB txt_dark = { 0, 0, 0 };
static const RGB txt_light = { 1, 1, 1 };

/* PlayerColors(1..20) from HexGrid, converted from VB's BGR values */
static const guint32 player_bgr[20] = {
    8421631, 65535, 33023, 16512, 32768, 16776960, 16744576, 16744703,
    16711935, 16711808, 128, 32896, 8421440, 12615680, 12615935, 8388736,
    8388672, 8454143, 11395647, 4830697
};

static RGB
bgr(guint32 v)
{
    RGB c = { (v & 0xff) / 255.0, ((v >> 8) & 0xff) / 255.0,
              ((v >> 16) & 0xff) / 255.0 };
    return c;
}

static RGB
from_rgba(const GdkRGBA *c)
{
    RGB r = { c->red, c->green, c->blue };
    return r;
}

static RGB
blend(RGB a, RGB b, double t)
{
    RGB r = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
              a.b + (b.b - a.b) * t };
    return r;
}

/* Black or white, whichever reads better on the fill */
static RGB
text_for(RGB fill)
{
    double lum = 0.299 * fill.r + 0.587 * fill.g + 0.114 * fill.b;

    return lum > 0.5 ? txt_dark : txt_light;
}

static void
sector_colours(const Sector *s, RGB *fill, RGB *text)
{
    if (!s || !s->des) {
        *fill = from_rgba(&scheme.unknown);
    } else if (s->des == '.' || s->des == '=') {
        *fill = from_rgba(&scheme.sea);
    } else if (scheme.mode == SCHEME_CONDITIONAL && (s->smark & SM_HAS)) {
        *fill = from_rgba(s->smark & SM_MET ? &scheme.met : &scheme.missed);
    } else if (scheme.mode == SCHEME_GRADUATED && (s->smark & SM_HAS)) {
        double t = scheme.high == scheme.low ? 1.0
            : (double)(s->sval - scheme.low) / (scheme.high - scheme.low);

        *fill = blend(from_rgba(&scheme.low_col), from_rgba(&scheme.high_col),
                      CLAMP(t, 0.0, 1.0));
    } else if (scheme.mode == SCHEME_DISTRIBUTION && (s->smark & SM_HAS)) {
        *fill = from_rgba(s->smark & SM_ROUTE ? &scheme.route : &scheme.noroute);
    } else if (s->own >= 0 && s->own == my_cnum) {
        *fill = from_rgba(&scheme.land);
    } else if (s->own > 0) {
        *fill = bgr(player_bgr[(s->own - 1) % 20]);
    } else {
        *fill = from_rgba(&scheme.other);
    }
    *text = text_for(*fill);
}

static double
delta(double a, double b, int size)
{
    double d = a - b;

    if (size > 0) {
        d = fmod(d, size);
        if (d < -size / 2.0)
            d += size;
        else if (d >= size / 2.0)
            d -= size;
    }
    return d;
}

static void
hex_path(cairo_t *cr, double x, double y, double s)
{
    cairo_move_to(cr, x, y - s);
    cairo_line_to(cr, x + SQRT3_2 * s, y - s / 2);
    cairo_line_to(cr, x + SQRT3_2 * s, y + s / 2);
    cairo_line_to(cr, x, y + s);
    cairo_line_to(cr, x - SQRT3_2 * s, y + s / 2);
    cairo_line_to(cr, x - SQRT3_2 * s, y - s / 2);
    cairo_close_path(cr);
}

static void
draw_text(cairo_t *cr, PangoLayout *lay, const char *t, double x, double y)
{
    int w, h;

    pango_layout_set_text(lay, t, -1);
    pango_layout_get_pixel_size(lay, &w, &h);
    cairo_move_to(cr, x - w / 2.0, y - h / 2.0);
    pango_cairo_show_layout(cr, lay);
}

/* Empire's six directions */
static gboolean
dir_delta(char d, int *dx, int *dy)
{
    switch (g_ascii_tolower(d)) {
    case 'y': *dx = -1; *dy = -1; return TRUE;
    case 'u': *dx = 1; *dy = -1; return TRUE;
    case 'g': *dx = -2; *dy = 0; return TRUE;
    case 'j': *dx = 2; *dy = 0; return TRUE;
    case 'b': *dx = -1; *dy = 1; return TRUE;
    case 'n': *dx = 1; *dy = 1; return TRUE;
    default: return FALSE;
    }
}

static double delta(double a, double b, int size);

/* The path as a line through the hex centres, with a dot at each end */
static void
draw_path(cairo_t *cr, int W, int H)
{
    double dx = SQRT3_2 * mv.side, dy = 1.5 * mv.side;
    double px = W / 2.0 + delta(mv.path_x, mv.cx, world_x) * dx;
    double py = H / 2.0 + delta(mv.path_y, mv.cy, world_y) * dy;
    int sx, sy;

    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, MAX(3.0, mv.side / 5));
    cairo_set_source_rgba(cr, 1, 0.1, 0.6, 0.85);
    cairo_arc(cr, px, py, MAX(3.0, mv.side / 4), 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_move_to(cr, px, py);
    for (const char *p = mv.path; *p; p++) {
        if (!dir_delta(*p, &sx, &sy))
            continue;
        px += sx * dx;
        py += sy * dy;
        cairo_line_to(cr, px, py);
    }
    cairo_stroke(cr);
    cairo_arc(cr, px, py, MAX(4.0, mv.side / 3), 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* Distribution scheme: a line from each sector to its distribution
 * sector, drawn the short way round the world */
static void
draw_dist_lines(cairo_t *cr, int W, int H)
{
    GArray *a = scheme_dist_lines();
    double dx = SQRT3_2 * mv.side, dy = 1.5 * mv.side;

    if (!a || !a->len)
        return;
    cairo_save(cr);
    cairo_set_line_width(cr, MAX(1.5, mv.side / 8));
    cairo_set_source_rgba(cr, 0.35, 0.1, 0.5, 0.8);
    for (guint i = 0; i + 3 < a->len; i += 4) {
        int x = g_array_index(a, int, i), y = g_array_index(a, int, i + 1);
        int tx = g_array_index(a, int, i + 2), ty = g_array_index(a, int, i + 3);
        double px = W / 2.0 + delta(x, mv.cx, world_x) * dx;
        double py = H / 2.0 + delta(y, mv.cy, world_y) * dy;
        double qx = px + delta(tx, x, world_x) * dx;
        double qy = py + delta(ty, y, world_y) * dy;

        cairo_move_to(cr, px, py);
        cairo_line_to(cr, qx, qy);
        cairo_stroke(cr);
        cairo_arc(cr, qx, qy, MAX(2.0, mv.side / 6), 0, 2 * G_PI);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

static void
swatch(cairo_t *cr, double x, double y, const GdkRGBA *c)
{
    cairo_rectangle(cr, x, y, 14, 12);
    gdk_cairo_set_source_rgba(cr, c);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

/* What the colours mean, in the bottom left corner */
static void
draw_legend(cairo_t *cr, int W, int H)
{
    char *title = scheme_legend();
    PangoLayout *lay;
    const char *a = NULL, *b = NULL;
    const GdkRGBA *ca = NULL, *cb = NULL;
    int tw, th, aw = 0, bw = 0, h;
    double x = 8, y, w;

    if (!title || world_x <= 0) {
        g_free(title);
        return;
    }
    lay = gtk_widget_create_pango_layout(mv.area, title);
    pango_layout_get_pixel_size(lay, &tw, &th);
    switch (scheme.mode) {
    case SCHEME_CONDITIONAL:
        a = "true"; ca = &scheme.met; b = "false"; cb = &scheme.missed;
        break;
    case SCHEME_GRADUATED:
        a = "low"; ca = &scheme.low_col; b = "high"; cb = &scheme.high_col;
        break;
    case SCHEME_DISTRIBUTION:
        a = "threshold"; ca = &scheme.route; b = "none"; cb = &scheme.noroute;
        break;
    }
    if (a) {
        PangoLayout *l2 = gtk_widget_create_pango_layout(mv.area, a);

        pango_layout_get_pixel_size(l2, &aw, NULL);
        pango_layout_set_text(l2, b, -1);
        pango_layout_get_pixel_size(l2, &bw, NULL);
        g_object_unref(l2);
    }
    w = MAX(tw, 14 + 4 + aw + 12 + 14 + 4 + bw) + 16;
    h = th + (a ? th + 6 : 0) + 12;
    y = H - h - 8;
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.88);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_move_to(cr, x + 8, y + 6);
    pango_cairo_show_layout(cr, lay);
    if (a) {
        double ly = y + 6 + th + 4, lx = x + 8;

        swatch(cr, lx, ly + (th - 12) / 2.0, ca);
        cairo_set_source_rgb(cr, 0, 0, 0);
        pango_layout_set_text(lay, a, -1);
        cairo_move_to(cr, lx + 18, ly);
        pango_cairo_show_layout(cr, lay);
        lx += 18 + aw + 12;
        swatch(cr, lx, ly + (th - 12) / 2.0, cb);
        cairo_set_source_rgb(cr, 0, 0, 0);
        pango_layout_set_text(lay, b, -1);
        cairo_move_to(cr, lx + 18, ly);
        pango_cairo_show_layout(cr, lay);
    }
    cairo_restore(cr);
    g_object_unref(lay);
    g_free(title);
}

void
mapview_set_pick(MapPickFunc fn, gpointer data)
{
    mv.pick = fn;
    mv.pick_data = data;
}

void
mapview_clear_pick(gpointer data)
{
    if (mv.pick_data == data) {
        mv.pick = NULL;
        mv.pick_data = NULL;
    }
}

void
mapview_show_path(int x, int y, const char *path)
{
    g_free(mv.path);
    mv.path = path && *path ? g_strdup(path) : NULL;
    mv.path_x = x;
    mv.path_y = y;
    mapview_redraw();
}

static gboolean
draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);
    double s = mv.side, dx = SQRT3_2 * s, dy = 1.5 * s;
    int cxi = (int)floor(mv.cx), cyi = (int)floor(mv.cy);
    int cols = (int)ceil(W / (2 * dx)) + 2, rows = (int)ceil(H / (2 * dy)) + 2;
    PangoLayout *big, *small;
    PangoFontDescription *fd;

    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_paint(cr);
    if (world_x <= 0 || world_y <= 0) {
        PangoLayout *lay = gtk_widget_create_pango_layout(w, NULL);

        cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
        draw_text(cr, lay, db_is_open()
                  ? "No map yet.\nConnect and use Refresh \342\206\222 Update All."
                  : "No game open.\nUse File \342\206\222 Open Game.", W / 2.0,
                  H / 2.0);
        g_object_unref(lay);
        return TRUE;
    }

    big = gtk_widget_create_pango_layout(w, NULL);
    fd = pango_font_description_from_string("Monospace Bold");
    pango_font_description_set_absolute_size(fd, s * 1.0 * PANGO_SCALE);
    pango_layout_set_font_description(big, fd);
    small = gtk_widget_create_pango_layout(w, NULL);
    pango_font_description_set_absolute_size(fd, s * 0.6 * PANGO_SCALE);
    pango_layout_set_font_description(small, fd);
    pango_font_description_free(fd);

    cairo_set_line_width(cr, s > 10 ? 1.0 : 0.5);
    for (int j = -rows; j <= rows; j++) {
        int y = cyi + j;
        double py = H / 2.0 + (y - mv.cy) * dy;

        if (py < -s || py > H + s)
            continue;
        for (int i = -2 * cols; i <= 2 * cols; i++) {
            int x = cxi + i;
            double px = W / 2.0 + (x - mv.cx) * dx;
            Sector *sec;
            RGB fill, text;
            gboolean sel;

            if ((x + y) & 1)
                continue;
            if (px < -dx || px > W + dx)
                continue;
            sec = world_sector(x, y);
            sector_colours(sec, &fill, &text);
            sel = mv.have_sel && world_wrap_x(x) == mv.sel_x
                && world_wrap_y(y) == mv.sel_y;
            if (sel) {
                RGB t = fill;
                fill = text;
                text = t;
            }
            hex_path(cr, px, py, s);
            cairo_set_source_rgb(cr, fill.r, fill.g, fill.b);
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
            cairo_stroke(cr);
            if (sel) {
                hex_path(cr, px, py, s - 1.5);
                cairo_set_source_rgb(cr, 1, 0.2, 0.2);
                cairo_set_line_width(cr, 2);
                cairo_stroke(cr);
                cairo_set_line_width(cr, s > 10 ? 1.0 : 0.5);
            }
            if (sec && s >= 10 && draw_unit_icons(cr, sec, px, py, s)) {
                /* designation goes below the icons, smaller */
                if (sec->des && sec->des != '.') {
                    char t[2] = { sec->des, 0 };

                    cairo_set_source_rgb(cr, text.r, text.g, text.b);
                    draw_text(cr, small, t, px, py + s * 0.45);
                }
            } else if (sec && sec->des && sec->des != '.' && s >= 7) {
                char t[2] = { sec->des, 0 };

                cairo_set_source_rgb(cr, text.r, text.g, text.b);
                if (sec->newdes && s >= 12) {
                    char n[2] = { sec->newdes, 0 };

                    draw_text(cr, big, t, px - s * 0.2, py - s * 0.1);
                    draw_text(cr, small, n, px + s * 0.4, py + s * 0.4);
                } else {
                    draw_text(cr, big, t, px, py);
                }
            }
            if (scheme.mode == SCHEME_GRADUATED && scheme.show_value && sec
                && (sec->smark & SM_HAS) && s >= 14) {
                char v[16];

                snprintf(v, sizeof(v), "%d", sec->sval);
                cairo_set_source_rgb(cr, text.r, text.g, text.b);
                draw_text(cr, small, v, px, py - s * 0.55);
            }
        }
    }
    g_object_unref(big);
    g_object_unref(small);
    draw_dist_lines(cr, W, H);
    if (mv.path)
        draw_path(cr, W, H);
    draw_legend(cr, W, H);
    return TRUE;
}

/* Which sector is under the pixel?  FALSE if none known. */
static gboolean
pixel_to_sector(double px, double py, int *sx, int *sy)
{
    int W = gtk_widget_get_allocated_width(mv.area);
    int H = gtk_widget_get_allocated_height(mv.area);
    double s = mv.side, fx = mv.cx + (px - W / 2.0) / (SQRT3_2 * s);
    double fy = mv.cy + (py - H / 2.0) / (1.5 * s);
    double best = 1e9;
    int bx = 0, by = 0;

    if (world_x <= 0)
        return FALSE;
    for (int y = (int)floor(fy) - 1; y <= (int)floor(fy) + 2; y++) {
        for (int x = (int)floor(fx) - 2; x <= (int)floor(fx) + 2; x++) {
            double ddx, ddy, d;

            if ((x + y) & 1)
                continue;
            ddx = (x - fx) * SQRT3_2;
            ddy = (y - fy) * 1.5;
            d = ddx * ddx + ddy * ddy;
            if (d < best) {
                best = d;
                bx = x;
                by = y;
            }
        }
    }
    *sx = world_wrap_x(bx);
    *sy = world_wrap_y(by);
    return TRUE;
}

void
mapview_redraw(void)
{
    if (mv.area)
        gtk_widget_queue_draw(mv.area);
}

static void
normalise_centre(void)
{
    if (world_x > 0) {
        mv.cx = delta(mv.cx, 0, world_x);
        mv.cy = delta(mv.cy, 0, world_y);
    }
}

void
mapview_center(int x, int y)
{
    mv.cx = x;
    mv.cy = y;
    normalise_centre();
    mapview_redraw();
}

void
mapview_zoom(double factor)
{
    mv.side = CLAMP(mv.side * factor, MIN_SIDE, MAX_SIDE);
    mapview_redraw();
}

void
mapview_select(int x, int y)
{
    mv.have_sel = TRUE;
    mv.sel_x = world_wrap_x(x);
    mv.sel_y = world_wrap_y(y);
    panel_show_sector(mv.sel_x, mv.sel_y);
    mapview_redraw();
}

gboolean
mapview_selected(int *x, int *y)
{
    if (mv.have_sel) {
        *x = mv.sel_x;
        *y = mv.sel_y;
    }
    return mv.have_sel;
}

/* -------------------------------------------------------- the menu */

static void
run_menu_cmd(GtkMenuItem *item, gpointer fmt)
{
    char *cmd = g_strdup_printf(fmt, mv.sel_x, mv.sel_y);

    conn_command(cmd, CMD_SHOW | CMD_USER, NULL, NULL, NULL);
    g_free(cmd);
}

static void
designate(GtkMenuItem *item, gpointer data)
{
    GtkWidget *dlg, *entry, *box;
    char *title = g_strdup_printf("Designate %d,%d", mv.sel_x, mv.sel_y);

    dlg = gtk_dialog_new_with_buttons(title, mainwin_window(),
                                      GTK_DIALOG_MODAL, "_Cancel",
                                      GTK_RESPONSE_CANCEL, "_Designate",
                                      GTK_RESPONSE_OK, NULL);
    g_free(title);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_set_spacing(GTK_BOX(box), 6);
    gtk_container_add(GTK_CONTAINER(box),
                      gtk_label_new("New designation (one letter, "
                                    "e.g. m for mine):"));
    entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry), 1);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_container_add(GTK_CONTAINER(box), entry);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *d = gtk_entry_get_text(GTK_ENTRY(entry));

        if (*d && *d != ' ' && *d != '"') {
            char *cmd = g_strdup_printf("designate %d,%d %c", mv.sel_x,
                                        mv.sel_y, *d);

            conn_command(cmd, CMD_SHOW | CMD_USER, NULL, NULL, NULL);
            g_free(cmd);
            /* pick up the change */
            refresh_sectors();
        }
    }
    gtk_widget_destroy(dlg);
}

/* Our units of a class in the selected sector, "3/4" */
static char *
units_here(UnitClass c)
{
    GPtrArray *a = world_units(c);
    GString *s = g_string_new(NULL);

    for (guint i = 0; i < a->len; i++) {
        Unit *u = g_ptr_array_index(a, i);

        if (u->x == mv.sel_x && u->y == mv.sel_y && u->carrier < 0)
            g_string_append_printf(s, "%s%d", s->len ? "/" : "", u->uid);
    }
    return g_string_free(s, FALSE);
}

static void
open_dialog(GtkMenuItem *item, gpointer which)
{
    char *u = NULL;

    switch (GPOINTER_TO_INT(which)) {
    case 1: dlg_move(mv.sel_x, mv.sel_y, TRUE); break;
    case 2: dlg_build(mv.sel_x, mv.sel_y, TRUE); break;
    case 3: dlg_nav(UC_SHIP, u = units_here(UC_SHIP)); break;
    case 4: dlg_load(UC_SHIP, u = units_here(UC_SHIP)); break;
    case 5: dlg_nav(UC_LAND, u = units_here(UC_LAND)); break;
    case 6: dlg_load(UC_LAND, u = units_here(UC_LAND)); break;
    case 7: dlg_distribute(mv.sel_x, mv.sel_y, TRUE); break;
    case 8: dlg_threshold(mv.sel_x, mv.sel_y, TRUE); break;
    case 9: dlg_attack(mv.sel_x, mv.sel_y, TRUE); break;
    case 10: dlg_fire(mv.sel_x, mv.sel_y, TRUE); break;
    case 11: dlg_air(mv.sel_x, mv.sel_y, TRUE); break;
    }
    g_free(u);
}

static void
center_here(GtkMenuItem *item, gpointer data)
{
    mapview_center(mv.sel_x, mv.sel_y);
}

static GtkWidget *
menu_item(GtkWidget *menu, const char *label, GCallback cb, gpointer data,
          gboolean needs_conn)
{
    GtkWidget *it = gtk_menu_item_new_with_mnemonic(label);

    g_signal_connect(it, "activate", cb, data);
    gtk_widget_set_sensitive(it, !needs_conn || conn_state() == CONN_PLAYING);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    return it;
}

static gboolean
destroy_menu_idle(gpointer menu)
{
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void
destroy_menu_later(GtkWidget *menu, gpointer d)
{
    g_idle_add(destroy_menu_idle, menu);
}

static void
popup_menu(GdkEvent *ev)
{
    GtkWidget *menu = gtk_menu_new();
    char *title = g_strdup_printf("Sector %d,%d", mv.sel_x, mv.sel_y);
    GtkWidget *t = gtk_menu_item_new_with_label(title);

    g_free(title);
    gtk_widget_set_sensitive(t, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), t);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, "_Census", G_CALLBACK(run_menu_cmd), "census %d,%d", TRUE);
    menu_item(menu, "C_ommodity", G_CALLBACK(run_menu_cmd), "commodity %d,%d",
              TRUE);
    menu_item(menu, "_Resource", G_CALLBACK(run_menu_cmd), "resource %d,%d",
              TRUE);
    menu_item(menu, "_Production", G_CALLBACK(run_menu_cmd),
              "production %d,%d", TRUE);
    menu_item(menu, "_Map around here", G_CALLBACK(run_menu_cmd),
              "map %d,%d", TRUE);
    menu_item(menu, "_Designate...", G_CALLBACK(designate), NULL, TRUE);
    menu_item(menu, "Mo_ve From Here...", G_CALLBACK(open_dialog),
              GINT_TO_POINTER(1), FALSE);
    menu_item(menu, "_Build Here...", G_CALLBACK(open_dialog),
              GINT_TO_POINTER(2), FALSE);
    {
        Sector *sec = world_sector(mv.sel_x, mv.sel_y);
        gboolean ours = sec && sec->own >= 0 && sec->own == my_cnum;

        if (ours) {
            menu_item(menu, "Dis_tribute...", G_CALLBACK(open_dialog),
                      GINT_TO_POINTER(7), FALSE);
            menu_item(menu, "T_hreshold...", G_CALLBACK(open_dialog),
                      GINT_TO_POINTER(8), FALSE);
        } else if (sec && sec->des) {
            menu_item(menu, "_Attack This Sector...", G_CALLBACK(open_dialog),
                      GINT_TO_POINTER(9), FALSE);
            menu_item(menu, "_Fire At This Sector...", G_CALLBACK(open_dialog),
                      GINT_TO_POINTER(10), FALSE);
        }
    }
    {
        static const char *unit_cmds[UC_COUNT][2] = {
            { "List S_hips Here", "ship %d,%d" },
            { "List _Land Units Here", "land %d,%d" },
            { "List Pla_nes Here", "plane %d,%d" },
            { "List N_ukes Here", "nuke %d,%d" },
        };
        Sector *sec = world_sector(mv.sel_x, mv.sel_y);
        gboolean sep = FALSE;

        for (int c = 0; c < UC_COUNT; c++) {
            if (!sec || !sec->units[c])
                continue;
            if (!sep) {
                gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                                      gtk_separator_menu_item_new());
                sep = TRUE;
            }
            menu_item(menu, unit_cmds[c][0], G_CALLBACK(run_menu_cmd),
                      (gpointer)unit_cmds[c][1], TRUE);
            if (c == UC_SHIP) {
                menu_item(menu, "Navigate Ships Here...",
                          G_CALLBACK(open_dialog), GINT_TO_POINTER(3), FALSE);
                menu_item(menu, "Load or Unload Ships Here...",
                          G_CALLBACK(open_dialog), GINT_TO_POINTER(4), FALSE);
            } else if (c == UC_LAND) {
                menu_item(menu, "March Land Units Here...",
                          G_CALLBACK(open_dialog), GINT_TO_POINTER(5), FALSE);
                menu_item(menu, "Load or Unload Land Units Here...",
                          G_CALLBACK(open_dialog), GINT_TO_POINTER(6), FALSE);
            } else if (c == UC_PLANE) {
                menu_item(menu, "Fly or Bomb With Planes Here...",
                          G_CALLBACK(open_dialog), GINT_TO_POINTER(11), FALSE);
            }
        }
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, "Cen_ter Map Here", G_CALLBACK(center_here), NULL, FALSE);
    gtk_widget_show_all(menu);
    /* GTK deactivates a menu before running the chosen item, so it may
     * only be destroyed once the main loop is idle again */
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu_later), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), ev);
}

/* ------------------------------------------------------------ input */

static gboolean
button_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    int x, y;

    gtk_widget_grab_focus(w);
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
        if (pixel_to_sector(ev->x, ev->y, &x, &y))
            mapview_center(x, y);
        return TRUE;
    }
    if (ev->type != GDK_BUTTON_PRESS)
        return FALSE;
    if (ev->button == 3) {
        if (pixel_to_sector(ev->x, ev->y, &x, &y)) {
            mapview_select(x, y);
            popup_menu((GdkEvent *)ev);
        }
        return TRUE;
    }
    mv.pressed = TRUE;
    mv.dragging = FALSE;
    mv.press_button = ev->button;
    mv.press_x = ev->x;
    mv.press_y = ev->y;
    mv.press_cx = mv.cx;
    mv.press_cy = mv.cy;
    return TRUE;
}

static gboolean
button_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    int x, y;

    if (!mv.pressed || ev->button != mv.press_button)
        return FALSE;
    mv.pressed = FALSE;
    if (!mv.dragging && ev->button == 1
        && pixel_to_sector(ev->x, ev->y, &x, &y)) {
        mapview_select(x, y);
        if (mv.pick)
            mv.pick(x, y, mv.pick_data);
    }
    if (mv.dragging) {
        normalise_centre();
        gdk_window_set_cursor(gtk_widget_get_window(w), NULL);
    }
    mv.dragging = FALSE;
    return TRUE;
}

static gboolean
motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    int x, y;

    if (mv.pressed) {
        double ddx = ev->x - mv.press_x, ddy = ev->y - mv.press_y;

        if (!mv.dragging && (fabs(ddx) > DRAG_SLOP || fabs(ddy) > DRAG_SLOP)) {
            GdkCursor *c = gdk_cursor_new_from_name(gtk_widget_get_display(w),
                                                    "grabbing");
            mv.dragging = TRUE;
            gdk_window_set_cursor(gtk_widget_get_window(w), c);
            if (c)
                g_object_unref(c);
        }
        if (mv.dragging) {
            mv.cx = mv.press_cx - ddx / (SQRT3_2 * mv.side);
            mv.cy = mv.press_cy - ddy / (1.5 * mv.side);
            mapview_redraw();
        }
    }
    status_hover(0, 0, FALSE);
    if (pixel_to_sector(ev->x, ev->y, &x, &y))
        status_hover(x, y, TRUE);
    return FALSE;
}

static gboolean
leave(GtkWidget *w, GdkEventCrossing *ev, gpointer data)
{
    status_hover(0, 0, FALSE);
    return FALSE;
}

static gboolean
scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data)
{
    double dx = 0, dy = 0;

    switch (ev->direction) {
    case GDK_SCROLL_UP: dy = -1; break;
    case GDK_SCROLL_DOWN: dy = 1; break;
    case GDK_SCROLL_LEFT: dx = -1; break;
    case GDK_SCROLL_RIGHT: dx = 1; break;
    case GDK_SCROLL_SMOOTH: dx = ev->delta_x; dy = ev->delta_y; break;
    default: break;
    }
    if (ev->state & GDK_CONTROL_MASK) {
        if (dy)
            mapview_zoom(dy < 0 ? 1.15 : 1 / 1.15);
        return TRUE;
    }
    if (ev->state & GDK_SHIFT_MASK) {
        double t = dx;
        dx = dy;
        dy = t;
    }
    mv.cx += dx * 3 * 2;
    mv.cy += dy * 3;
    normalise_centre();
    mapview_redraw();
    return TRUE;
}

static gboolean
key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    int x = mv.sel_x, y = mv.sel_y;

    switch (ev->keyval) {
    case GDK_KEY_plus: case GDK_KEY_KP_Add: case GDK_KEY_equal:
        mapview_zoom(1.25);
        return TRUE;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
        mapview_zoom(0.8);
        return TRUE;
    case GDK_KEY_Left: x -= 2; break;
    case GDK_KEY_Right: x += 2; break;
    case GDK_KEY_Up: x -= 1; y -= 1; break;
    case GDK_KEY_Down: x += 1; y += 1; break;
    default:
        return FALSE;
    }
    if (!mv.have_sel) {
        x = (int)round(mv.cx);
        y = (int)round(mv.cy);
        if ((x + y) & 1)
            x++;
    }
    mapview_select(x, y);
    {
        int W = gtk_widget_get_allocated_width(w);
        int H = gtk_widget_get_allocated_height(w);
        double px = W / 2.0 + delta(mv.sel_x, mv.cx, world_x) * SQRT3_2
            * mv.side;
        double py = H / 2.0 + delta(mv.sel_y, mv.cy, world_y) * 1.5 * mv.side;

        if (px < mv.side * 2 || px > W - mv.side * 2 || py < mv.side * 2
            || py > H - mv.side * 2)
            mapview_center(mv.sel_x, mv.sel_y);
    }
    return TRUE;
}

GtkWidget *
mapview_new(void)
{
    mv.area = gtk_drawing_area_new();
    gtk_widget_set_can_focus(mv.area, TRUE);
    gtk_widget_add_events(mv.area, GDK_BUTTON_PRESS_MASK
                          | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK
                          | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK
                          | GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK);
    gtk_widget_set_size_request(mv.area, 300, 200);
    g_signal_connect(mv.area, "draw", G_CALLBACK(draw), NULL);
    g_signal_connect(mv.area, "button-press-event", G_CALLBACK(button_press),
                     NULL);
    g_signal_connect(mv.area, "button-release-event",
                     G_CALLBACK(button_release), NULL);
    g_signal_connect(mv.area, "motion-notify-event", G_CALLBACK(motion), NULL);
    g_signal_connect(mv.area, "leave-notify-event", G_CALLBACK(leave), NULL);
    g_signal_connect(mv.area, "scroll-event", G_CALLBACK(scroll), NULL);
    g_signal_connect(mv.area, "key-press-event", G_CALLBACK(key_press), NULL);
    return mv.area;
}
