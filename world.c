/*
 * world.c - what we know about the world, assembled from the database
 * for drawing (WinACE HexGrid LoadScreen).
 *
 * Coordinates are as the server shows them to this country: relative
 * to its capital origin, x in [-WORLD_X/2, WORLD_X/2), y likewise.
 * Only sectors with x + y even exist.
 */
#include <string.h>
#include <sqlite3.h>
#include "gtkace.h"

int world_x, world_y;
int my_cnum = -1;

static Sector *grid;
static GHashTable *countries;   /* cnum -> name */
static char *des_names[128];    /* mnemonic -> name */
static int owned;

static const char *unit_tables[UC_COUNT] = { "ship", "land", "plane", "nuke" };
static const char *unit_nouns[UC_COUNT][2] = {
    { "ship", "ships" }, { "land unit", "land units" },
    { "plane", "planes" }, { "nuke", "nukes" }
};
static GPtrArray *units[UC_COUNT];      /* Unit * */
static GHashTable *unit_mnem[UC_COUNT];  /* type -> "dd" */
static GHashTable *unit_tname[UC_COUNT]; /* type -> "destroyer" */

const char *
world_unit_table(UnitClass c)
{
    return unit_tables[c];
}

const char *
world_unit_noun(UnitClass c, gboolean plural)
{
    return unit_nouns[c][plural ? 1 : 0];
}

GPtrArray *
world_units(UnitClass c)
{
    if (!units[c])
        units[c] = g_ptr_array_new_with_free_func(g_free);
    return units[c];
}

const char *
world_unit_mnem(UnitClass c, int type)
{
    const char *s = unit_mnem[c]
        ? g_hash_table_lookup(unit_mnem[c], GINT_TO_POINTER(type)) : NULL;

    return s ? s : "?";
}

const char *
world_unit_type_name(UnitClass c, int type)
{
    const char *s = unit_tname[c]
        ? g_hash_table_lookup(unit_tname[c], GINT_TO_POINTER(type)) : NULL;

    return s ? s : "";
}

char *
world_unit_describe(const Unit *u)
{
    static const char *grp[UC_COUNT] = { "fleet", "army", "wing", "" };
    GString *s = g_string_new(NULL);

    g_string_printf(s, "%s #%d  %s %s  %d%%", world_unit_noun(u->cls, FALSE),
                    u->uid, world_unit_mnem(u->cls, u->type),
                    world_unit_type_name(u->cls, u->type), u->eff);
    if (u->group)
        g_string_append_printf(s, "  %s %c", grp[u->cls], u->group);
    if (u->carrier >= 0)
        g_string_append_printf(s, "  on %s #%d",
                               u->carrier_cls == 's' ? "ship"
                               : u->carrier_cls == 'p' ? "plane" : "land unit",
                               u->carrier);
    return g_string_free(s, FALSE);
}

static sqlite3_stmt *prepare(const char *sql);

/* "dd   destroyer" -> mnemonic "dd", name "destroyer" */
static void
load_unit_types(UnitClass c)
{
    char *tbl = g_strdup_printf("%s-chr", unit_tables[c]);
    char *sql;
    sqlite3_stmt *st;

    if (!unit_mnem[c]) {
        unit_mnem[c] = g_hash_table_new_full(NULL, NULL, NULL, g_free);
        unit_tname[c] = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    }
    g_hash_table_remove_all(unit_mnem[c]);
    g_hash_table_remove_all(unit_tname[c]);
    if (!db_table_exists(tbl)) {
        g_free(tbl);
        return;
    }
    sql = g_strdup_printf("SELECT type, name FROM \"%s\"", tbl);
    if ((st = prepare(sql))) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            int type = sqlite3_column_int(st, 0);
            const char *n = (const char *)sqlite3_column_text(st, 1);
            const char *sp;

            if (!n)
                continue;
            sp = strchr(n, ' ');
            g_hash_table_insert(unit_mnem[c], GINT_TO_POINTER(type),
                                sp ? g_strndup(n, sp - n) : g_strdup(n));
            if (sp) {
                while (*sp == ' ')
                    sp++;
                g_hash_table_insert(unit_tname[c], GINT_TO_POINTER(type),
                                    g_strdup(sp));
            }
        }
        sqlite3_finalize(st);
    }
    g_free(sql);
    g_free(tbl);
}

/* Columns differ between classes and server versions: look them up */
static int
col_index(sqlite3_stmt *st, const char *name)
{
    int n = sqlite3_column_count(st);

    for (int i = 0; i < n; i++)
        if (!strcmp(sqlite3_column_name(st, i), name))
            return i;
    return -1;
}

static int
col_int(sqlite3_stmt *st, int i, int dflt)
{
    return i < 0 || sqlite3_column_type(st, i) == SQLITE_NULL
        ? dflt : sqlite3_column_int(st, i);
}

static void
load_units(UnitClass c)
{
    static const char *grp_col[UC_COUNT] = { "fleet", "army", "wing", NULL };
    GPtrArray *a = world_units(c);
    sqlite3_stmt *st;
    char *sql;

    g_ptr_array_set_size(a, 0);
    load_unit_types(c);
    if (!db_table_exists(unit_tables[c]))
        return;
    sql = g_strdup_printf("SELECT * FROM \"%s\" ORDER BY uid", unit_tables[c]);
    st = prepare(sql);
    g_free(sql);
    if (!st)
        return;
    {
        int iuid = col_index(st, "uid"), iown = col_index(st, "owner");
        int ix = col_index(st, "xloc"), iy = col_index(st, "yloc");
        int itype = col_index(st, "type"), ieff = col_index(st, "effic");
        int imob = col_index(st, "mobil"), itech = col_index(st, "tech");
        int igrp = grp_col[c] ? col_index(st, grp_col[c]) : -1;
        int iship = col_index(st, "ship"), iland = col_index(st, "land");
        int iplane = col_index(st, "plane");

        while (sqlite3_step(st) == SQLITE_ROW) {
            Unit *u = g_new0(Unit, 1);
            Sector *s;
            const char *g;

            u->cls = c;
            u->uid = col_int(st, iuid, -1);
            u->owner = col_int(st, iown, -1);
            u->x = col_int(st, ix, 0);
            u->y = col_int(st, iy, 0);
            u->type = col_int(st, itype, -1);
            u->eff = col_int(st, ieff, 0);
            u->mob = col_int(st, imob, 0);
            u->tech = col_int(st, itech, 0);
            g = igrp >= 0 ? (const char *)sqlite3_column_text(st, igrp) : NULL;
            u->group = g && g[0] && g[0] != ' ' && g[0] != '~' ? g[0] : 0;
            u->carrier = -1;
            if (c == UC_LAND || c == UC_PLANE) {
                /* carried by a ship, or by a land unit */
                if (col_int(st, iship, -1) >= 0) {
                    u->carrier = col_int(st, iship, -1);
                    u->carrier_cls = 's';
                } else if (col_int(st, iland, -1) >= 0) {
                    u->carrier = col_int(st, iland, -1);
                    u->carrier_cls = 'l';
                }
            } else if (c == UC_NUKE && col_int(st, iplane, -1) >= 0) {
                u->carrier = col_int(st, iplane, -1);
                u->carrier_cls = 'p';
            }
            if (u->uid < 0 || u->type < 0) {
                g_free(u);
                continue;
            }
            g_ptr_array_add(a, u);
            s = world_sector(u->x, u->y);
            if (s && s->units[c] < 255)
                s->units[c]++;
        }
        sqlite3_finalize(st);
    }
}

int
world_wrap_x(int x)
{
    if (world_x <= 0)
        return x;
    x = ((x + world_x / 2) % world_x + world_x) % world_x;
    return x - world_x / 2;
}

int
world_wrap_y(int y)
{
    if (world_y <= 0)
        return y;
    y = ((y + world_y / 2) % world_y + world_y) % world_y;
    return y - world_y / 2;
}

Sector *
world_sector(int x, int y)
{
    if (!grid || world_x <= 0 || world_y <= 0)
        return NULL;
    x = world_wrap_x(x);
    y = world_wrap_y(y);
    if ((x + y) & 1)
        return NULL;
    return &grid[(y + world_y / 2) * world_x + (x + world_x / 2)];
}

const char *
world_country_name(int cnum)
{
    const char *n = countries
        ? g_hash_table_lookup(countries, GINT_TO_POINTER(cnum)) : NULL;

    return n ? n : "";
}

const char *
world_des_name(char mnem)
{
    unsigned char c = mnem;

    return c < 128 && des_names[c] ? des_names[c] : "";
}

int
world_sector_count(void)
{
    return owned;
}

static sqlite3_stmt *
prepare(const char *sql)
{
    sqlite3_stmt *st = NULL;

    if (sqlite3_prepare_v2(db_handle(), sql, -1, &st, NULL) != SQLITE_OK)
        return NULL;
    return st;
}

void
world_load(void)
{
    sqlite3_stmt *st;
    char *country;
    char mnem_by_uid[256];

    g_free(grid);
    grid = NULL;
    owned = 0;
    for (int i = 0; i < 128; i++)
        g_clear_pointer(&des_names[i], g_free);
    if (countries)
        g_hash_table_remove_all(countries);
    else
        countries = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    world_x = world_y = 0;
    my_cnum = -1;
    for (int c = 0; c < UC_COUNT; c++)
        g_ptr_array_set_size(world_units(c), 0);
    if (!db_is_open())
        return;

    if (db_table_exists("version")) {
        world_x = db_get_int("SELECT \"WORLD_X\" FROM version", 0);
        world_y = db_get_int("SELECT \"WORLD_Y\" FROM version", 0);
    }
    if (world_x <= 0 || world_y <= 0 || world_x > 4096 || world_y > 4096
        || (world_x & 1) || (world_y & 1)) {
        world_x = world_y = 0;
        return;
    }
    grid = g_new0(Sector, (gsize)world_x * world_y);
    for (int i = 0; i < world_x * world_y; i++)
        grid[i].own = -1;

    /* countries */
    if (db_table_exists("nat") && (st = prepare("SELECT cnum, cname FROM nat"))) {
        while (sqlite3_step(st) == SQLITE_ROW)
            g_hash_table_insert(countries,
                                GINT_TO_POINTER(sqlite3_column_int(st, 0)),
                                g_strdup((const char *)sqlite3_column_text(st, 1)));
        sqlite3_finalize(st);
    }
    country = db_get_setting("country");
    if (country) {
        GHashTableIter it;
        gpointer k, v;

        g_hash_table_iter_init(&it, countries);
        while (g_hash_table_iter_next(&it, &k, &v))
            if (!g_ascii_strcasecmp(v, country))
                my_cnum = GPOINTER_TO_INT(k);
        g_free(country);
    }
    if (my_cnum < 0 && db_table_exists("country")
        && db_get_int("SELECT count(*) FROM country", 0) == 1)
        my_cnum = db_get_int("SELECT cnum FROM country", -1);

    /* sector types */
    memset(mnem_by_uid, 0, sizeof(mnem_by_uid));
    if (db_table_exists("sect-chr")
        && (st = prepare("SELECT uid, mnem, name FROM \"sect-chr\""))) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            int uid = sqlite3_column_int(st, 0);
            const char *m = (const char *)sqlite3_column_text(st, 1);
            const char *n = (const char *)sqlite3_column_text(st, 2);

            if (uid >= 0 && uid < 256 && m && m[0] && !m[1]
                && (unsigned char)m[0] < 128) {
                mnem_by_uid[uid] = m[0];
                g_free(des_names[(unsigned char)m[0]]);
                des_names[(unsigned char)m[0]] = g_strdup(n ? n : "");
            }
        }
        sqlite3_finalize(st);
    }

    /* what bmap shows of the world */
    if ((st = prepare("SELECT x, y, des FROM bmap"))) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            Sector *s = world_sector(sqlite3_column_int(st, 0),
                                     sqlite3_column_int(st, 1));
            const char *d = (const char *)sqlite3_column_text(st, 2);

            if (s && d && d[0])
                s->des = d[0];
        }
        sqlite3_finalize(st);
    }

    /* our own sectors in full */
    if (db_table_exists("sect")
        && (st = prepare("SELECT xloc, yloc, owner, des, newdes, effic "
                         "FROM sect"))) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            Sector *s = world_sector(sqlite3_column_int(st, 0),
                                     sqlite3_column_int(st, 1));
            int des = sqlite3_column_int(st, 3);
            int newdes = sqlite3_column_int(st, 4);

            if (!s)
                continue;
            s->own = sqlite3_column_int(st, 2);
            if (des >= 0 && des < 256 && mnem_by_uid[des])
                s->des = mnem_by_uid[des];
            s->newdes = newdes != des && newdes >= 0 && newdes < 256
                ? mnem_by_uid[newdes] : 0;
            s->eff = CLAMP(sqlite3_column_int(st, 5), 0, 100);
            if (s->own == my_cnum)
                owned++;
        }
        sqlite3_finalize(st);
    }

    for (int c = 0; c < UC_COUNT; c++)
        load_units(c);
}
