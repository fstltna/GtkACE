/*
 * intel.c - intelligence on other countries, gathered from the output
 * of commands (WinACE EmpireParse ParseLook, ParseCoastWatch,
 * ParseSonar, ParseSpyReport, and the enemy unit/sector tables).
 *
 * Every line of output from commands the player runs is checked, so
 * sightings are recorded whichever command produced them: lookout,
 * llook, coastwatch, skywatch, sonar, spy, and others that print the
 * same kind of line.  Also radar maps (radar, lradar, satellite),
 * satellite and spy plane reports, reconnaissance flights (WinACE
 * ParseReconReport) and what attack, assault and paradrop say of their
 * target (ParseAttack).
 */
#include <string.h>
#include <stdlib.h>
#include <sqlite3.h>
#include "gtkace.h"

static struct {
    char source[16];            /* command that produced the output */
    gboolean in_spy;            /* inside a spy report table */
    gboolean in_sat;            /* satellite or spy plane sector rows */
    int sat_cls;                /* satellite unit rows: UC_SHIP, UC_LAND */
    /* radar maps: "x,y efficiency N%, max range R" then the map */
    gboolean rad_pending, rad_wait, rad_active;
    int rad_x, rad_y, rad_range, rad_row;
    gboolean changed;
    gboolean in_txn;
} in;

static gint64
now(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

static gboolean
ensure_tables(void)
{
    sqlite3 *db = db_handle();

    if (!db)
        return FALSE;
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS intel_sect (x INTEGER, y INTEGER, "
                 "owner INTEGER, des TEXT, newdes TEXT, eff INTEGER, "
                 "civ INTEGER, mil INTEGER, shell INTEGER, gun INTEGER, "
                 "petrol INTEGER, food INTEGER, bar INTEGER, road INTEGER, "
                 "rail INTEGER, defense INTEGER, oldown INTEGER, "
                 "source TEXT, seen INTEGER, PRIMARY KEY (x, y));"
                 "CREATE TABLE IF NOT EXISTS intel_unit (cls INTEGER, "
                 "uid INTEGER, owner INTEGER, type INTEGER, x INTEGER, "
                 "y INTEGER, eff INTEGER, mil INTEGER, source TEXT, "
                 "seen INTEGER, PRIMARY KEY (cls, uid));"
                 "CREATE TABLE IF NOT EXISTS intel_contact (x INTEGER, "
                 "y INTEGER, mark TEXT, what TEXT, owner INTEGER, "
                 "source TEXT, seen INTEGER, PRIMARY KEY (x, y));",
                 NULL, NULL, NULL);
    /* added in 0.10.0; fails harmlessly once the column is there */
    sqlite3_exec(db, "ALTER TABLE intel_sect ADD COLUMN iron INTEGER",
                 NULL, NULL, NULL);
    return TRUE;
}

static void
begin(void)
{
    if (!in.in_txn && ensure_tables()) {
        sqlite3_exec(db_handle(), "BEGIN", NULL, NULL, NULL);
        in.in_txn = TRUE;
    }
}

/* ------------------------------------------------------------ store */

/* NULL-able integers: INT_UNKNOWN means "this report didn't say" */
#define INT_UNKNOWN G_MININT

static void
bind_int_or_null(sqlite3_stmt *st, int col, int v)
{
    if (v == INT_UNKNOWN)
        sqlite3_bind_null(st, col);
    else
        sqlite3_bind_int(st, col, v);
}

typedef struct {
    int x, y, owner, oldown;
    char des, newdes;
    int eff, civ, mil, shell, gun, petrol, iron, food, bar, road, rail,
        defense;
} SectIntel;

static void
sect_intel_init(SectIntel *s)
{
    s->x = s->y = 0;
    s->des = s->newdes = 0;
    s->owner = s->oldown = s->eff = s->civ = s->mil = s->shell = s->gun
        = s->petrol = s->iron = s->food = s->bar = s->road = s->rail = s->defense
        = INT_UNKNOWN;
}

static void
store_sector(const SectIntel *s)
{
    sqlite3_stmt *st;
    char d[2] = { s->des, 0 }, nd[2] = { s->newdes, 0 };

    if (s->owner != INT_UNKNOWN && s->owner == my_cnum)
        return;                 /* our own sectors come from xdump */
    begin();
    /* Fields this report did not include keep their old values, unless
     * the sector changed hands, when they are no longer meaningful. */
    if (sqlite3_prepare_v2(db_handle(),
        "INSERT INTO intel_sect (x, y, owner, des, newdes, eff, civ, mil, "
        "shell, gun, petrol, food, bar, road, rail, defense, oldown, source, "
        "seen, iron) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT (x, y) DO UPDATE SET "
        "owner = coalesce(excluded.owner, owner), "
        "des = coalesce(excluded.des, des), "
        "newdes = excluded.newdes, "
        "eff = coalesce(excluded.eff, eff), "
        "civ = CASE WHEN excluded.owner IS NOT owner THEN excluded.civ "
        "      ELSE coalesce(excluded.civ, civ) END, "
        "mil = CASE WHEN excluded.owner IS NOT owner THEN excluded.mil "
        "      ELSE coalesce(excluded.mil, mil) END, "
        "shell = coalesce(excluded.shell, shell), "
        "gun = coalesce(excluded.gun, gun), "
        "petrol = coalesce(excluded.petrol, petrol), "
        "food = coalesce(excluded.food, food), "
        "bar = coalesce(excluded.bar, bar), "
        "road = coalesce(excluded.road, road), "
        "rail = coalesce(excluded.rail, rail), "
        "defense = coalesce(excluded.defense, defense), "
        "oldown = coalesce(excluded.oldown, oldown), "
        "iron = coalesce(excluded.iron, iron), "
        "source = excluded.source, seen = excluded.seen",
        -1, &st, NULL) != SQLITE_OK) {
        g_warning("intel: %s", sqlite3_errmsg(db_handle()));
        return;
    }
    sqlite3_bind_int(st, 1, s->x);
    sqlite3_bind_int(st, 2, s->y);
    bind_int_or_null(st, 3, s->owner);
    if (s->des)
        sqlite3_bind_text(st, 4, d, 1, SQLITE_TRANSIENT);
    if (s->newdes && s->newdes != ' ')
        sqlite3_bind_text(st, 5, nd, 1, SQLITE_TRANSIENT);
    bind_int_or_null(st, 6, s->eff);
    bind_int_or_null(st, 7, s->civ);
    bind_int_or_null(st, 8, s->mil);
    bind_int_or_null(st, 9, s->shell);
    bind_int_or_null(st, 10, s->gun);
    bind_int_or_null(st, 11, s->petrol);
    bind_int_or_null(st, 12, s->food);
    bind_int_or_null(st, 13, s->bar);
    bind_int_or_null(st, 14, s->road);
    bind_int_or_null(st, 15, s->rail);
    bind_int_or_null(st, 16, s->defense);
    bind_int_or_null(st, 17, s->oldown);
    sqlite3_bind_text(st, 18, in.source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 19, now());
    bind_int_or_null(st, 20, s->iron);
    sqlite3_step(st);
    sqlite3_finalize(st);
    in.changed = TRUE;
}

static void
store_unit(int cls, int uid, int owner, int type, int x, int y, int mil,
           int eff)
{
    sqlite3_stmt *st;

    if (owner >= 0 && owner == my_cnum)
        return;
    begin();
    if (sqlite3_prepare_v2(db_handle(),
        "INSERT INTO intel_unit (cls, uid, owner, type, x, y, eff, mil, "
        "source, seen) VALUES (?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT (cls, uid) DO UPDATE SET "
        "owner = CASE WHEN excluded.owner < 0 THEN owner "
        "        ELSE excluded.owner END, "
        "type = excluded.type, x = excluded.x, y = excluded.y, "
        "eff = coalesce(excluded.eff, eff), "
        "mil = coalesce(excluded.mil, mil), "
        "source = excluded.source, seen = excluded.seen",
        -1, &st, NULL) != SQLITE_OK) {
        g_warning("intel: %s", sqlite3_errmsg(db_handle()));
        return;
    }
    sqlite3_bind_int(st, 1, cls);
    sqlite3_bind_int(st, 2, uid);
    sqlite3_bind_int(st, 3, owner);
    sqlite3_bind_int(st, 4, type);
    sqlite3_bind_int(st, 5, x);
    sqlite3_bind_int(st, 6, y);
    bind_int_or_null(st, 7, eff);
    bind_int_or_null(st, 8, mil);
    sqlite3_bind_text(st, 9, in.source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 10, now());
    sqlite3_step(st);
    sqlite3_finalize(st);
    in.changed = TRUE;
}

/* Something seen that can't be identified: a radar blip, ships or land
 * units a reconnaissance plane flew over.  One per sector; mark is what
 * the map shows (the radar letter, $ for a satellite). */
static void
store_contact(int x, int y, char mark, const char *what, int owner)
{
    sqlite3_stmt *st;
    char m[2] = { mark, 0 };

    begin();
    if (sqlite3_prepare_v2(db_handle(), "INSERT OR REPLACE INTO intel_contact "
                           "(x, y, mark, what, owner, source, seen) "
                           "VALUES (?,?,?,?,?,?,?)", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int(st, 1, x);
    sqlite3_bind_int(st, 2, y);
    sqlite3_bind_text(st, 3, m, 1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, what, -1, SQLITE_TRANSIENT);
    if (owner >= 0)
        sqlite3_bind_int(st, 5, owner);
    sqlite3_bind_text(st, 6, in.source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 7, now());
    sqlite3_step(st);
    sqlite3_finalize(st);
    in.changed = TRUE;
}

/* A radar swept this sector and saw nothing there now */
static void
forget_contact(int x, int y)
{
    sqlite3_stmt *st;

    begin();
    if (sqlite3_prepare_v2(db_handle(), "DELETE FROM intel_contact WHERE "
                           "x=? AND y=?", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int(st, 1, x);
    sqlite3_bind_int(st, 2, y);
    sqlite3_step(st);
    if (sqlite3_changes(db_handle()))
        in.changed = TRUE;
    sqlite3_finalize(st);
}

/* ---------------------------------------------------------- parsing */

/* "... @ x,y" at the end of a line */
static gboolean
parse_at(const char *line, int *x, int *y, const char **at)
{
    const char *p = g_strrstr(line, " @ ");
    char *end;

    if (!p)
        return FALSE;
    *x = strtol(p + 3, &end, 10);
    if (end == p + 3 || *end != ',')
        return FALSE;
    *y = strtol(end + 1, &end, 10);
    while (*end == ' ')
        end++;
    if (*end)
        return FALSE;
    *at = p;
    return TRUE;
}

/* "name (#n) " -> n, and where the text after it starts.  Coastwatch
 * pads the number: "(#  2)". */
static gboolean
parse_natid(const char *line, const char *limit, int *cnum, const char **rest)
{
    const char *p = strstr(line, "(#");
    char *end;

    if (!p || p >= limit)
        return FALSE;
    *cnum = strtol(p + 2, &end, 10);
    if (end == p + 2 || *end != ')' || end[1] != ' ')
        return FALSE;
    *rest = end + 2;
    while (**rest == ' ')
        (*rest)++;
    return TRUE;
}

/* Unit description: "dd   destroyer (#4)", "dd   destroyer Name(#4)",
 * "inf  infantry #3", "f1   Sopwith Camel #0".  Sets class, type and
 * uid; end is where the description must finish. */
static gboolean
parse_unit_desc(const char *p, const char *end, int *cls, int *type, int *uid)
{
    int len;
    const char *q;
    char *e;

    *cls = world_match_unit_type(p, type, &len);
    if (*cls < 0 || *cls == UC_NUKE)
        return FALSE;
    while (end > p && end[-1] == ' ')
        end--;
    if (*cls == UC_SHIP) {
        /* the ship's own name may come between type and number */
        if (end - p < 4 || end[-1] != ')')
            return FALSE;
        for (q = end - 2; q > p && g_ascii_isdigit(*q); q--)
            ;
        if (q[0] != '#' || q[-1] != '(')
            return FALSE;
        *uid = strtol(q + 1, &e, 10);
        return TRUE;
    }
    q = p + len;
    while (*q == ' ')
        q++;
    if (*q != '#' || !g_ascii_isdigit(q[1]))
        return FALSE;
    *uid = strtol(q + 1, &e, 10);
    return e <= end;
}

/* "<natid> <sector type> N% efficient [with [approx ]N civ ]
 *  [with [approx ]N mil ]@ x,y" (lookout, llook, look) */
static gboolean
try_sector(const char *line, const char *at, int x, int y)
{
    const char *rest, *pct, *eff = strstr(line, "% efficient");
    int owner;
    char *name, *sp;
    SectIntel s;

    if (!eff || eff > at || g_str_has_prefix(line, "Your "))
        return FALSE;
    if (!parse_natid(line, eff, &owner, &rest))
        return FALSE;
    /* the efficiency number is the word before "%" */
    for (pct = eff; pct > rest && pct[-1] != ' '; pct--)
        ;
    if (pct <= rest)
        return FALSE;
    name = g_strndup(rest, pct - 1 - rest);
    sect_intel_init(&s);
    s.x = x;
    s.y = y;
    s.owner = owner;
    s.des = world_des_by_name(name);
    g_free(name);
    if (!s.des)
        return FALSE;
    s.eff = atoi(pct);
    /* civ and mil are absent when there are none */
    s.civ = s.mil = 0;
    if ((sp = strstr(eff, " civ")) && sp < at) {
        const char *n = sp;

        while (n > eff && n[-1] != ' ')
            n--;
        s.civ = atoi(n);
    }
    if ((sp = strstr(eff, " mil")) && sp < at) {
        const char *n = sp;

        while (n > eff && n[-1] != ' ')
            n--;
        s.mil = atoi(n);
    }
    store_sector(&s);
    return TRUE;
}

/* "<natid> <unit> [(approx N mil)] @ x,y" (lookout, coastwatch,
 * skywatch, llook) */
static gboolean
try_unit(const char *line, const char *at, int x, int y)
{
    const char *rest, *end = at, *ap;
    int owner, cls, type, uid, mil = INT_UNKNOWN;

    if (!parse_natid(line, at, &owner, &rest) || rest >= at)
        return FALSE;
    if ((ap = strstr(rest, " (approx ")) && ap < at) {
        mil = atoi(ap + 9);
        end = ap;
    }
    if (!parse_unit_desc(rest, end, &cls, &type, &uid))
        return FALSE;
    store_unit(cls, uid, owner, type, x, y, mil, INT_UNKNOWN);
    return TRUE;
}

/* "Sonar detects [<country> ]<ship> @ x,y" */
static gboolean
try_sonar(const char *line, const char *at, int x, int y)
{
    const char *p = line + strlen("Sonar detects ");
    int cls, type, uid;

    if (!g_str_has_prefix(line, "Sonar detects "))
        return FALSE;
    /* find where the ship description starts; before it is the owner */
    for (const char *q = p; q < at; q++) {
        if (q != p && q[-1] != ' ')
            continue;
        if (parse_unit_desc(q, at, &cls, &type, &uid) && cls == UC_SHIP) {
            int owner = -1;

            if (q > p) {
                char *name = g_strndup(p, q - 1 - p);

                owner = world_country_by_name(g_strstrip(name));
                g_free(name);
            }
            store_unit(cls, uid, owner, type, x, y, INT_UNKNOWN, INT_UNKNOWN);
            return TRUE;
        }
    }
    return FALSE;
}

/* Spy report rows:
 *    sect   de own own eff eff eff eff  civ  mil  shl gun  pet food bars
 *   -4,4    h    2   2 100   0   0   0 1010  120  900 900  900  900    0
 * The coordinates take 9 columns ("%4d,%-4d"), then a space, then the
 * designation and the new designation (blank if unchanged). */
static gboolean
try_spy_row(const char *line)
{
    int x, y, v[13], n;
    char *end;
    SectIntel s;

    x = strtol(line, &end, 10);
    if (end == line || *end != ',' || strlen(line) < 13)
        return FALSE;
    y = strtol(end + 1, &end, 10);
    if (line[9] != ' ' || line[10] == ' ')
        return FALSE;
    n = sscanf(line + 12, "%d %d %d %d %d %d %d %d %d %d %d %d %d", &v[0],
               &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9],
               &v[10], &v[11], &v[12]);
    if (n != 13)
        return FALSE;
    sect_intel_init(&s);
    s.x = x;
    s.y = y;
    s.des = line[10];
    s.newdes = line[11];
    s.owner = v[0];
    s.oldown = v[1];
    s.eff = v[2];
    s.road = v[3];
    s.rail = v[4];
    s.defense = v[5];
    s.civ = v[6];
    s.mil = v[7];
    s.shell = v[8];
    s.gun = v[9];
    s.petrol = v[10];
    s.food = v[11];
    s.bar = v[12];
    store_sector(&s);
    return TRUE;
}

/* "Neutral (name) unit in x,y: inf  infantry #3 ..."
 * "At War (name) plane in x,y: f1   Sopwith Camel #0" */
static gboolean
try_spy_unit(const char *line)
{
    const char *op = strchr(line, '('), *cp, *kind, *colon;
    int x, y, cls, type, uid, owner;
    char *name, *end;

    if (!op || !(cp = strstr(op, ") ")))
        return FALSE;
    kind = cp + 2;
    if (g_str_has_prefix(kind, "unit in "))
        kind += 8;
    else if (g_str_has_prefix(kind, "plane in "))
        kind += 9;
    else
        return FALSE;
    x = strtol(kind, &end, 10);
    if (end == kind || *end != ',')
        return FALSE;
    y = strtol(end + 1, &end, 10);
    if (*end != ':' || end[1] != ' ')
        return FALSE;
    colon = end + 2;
    /* land units and planes: "type name #uid", maybe more after it */
    if (!parse_unit_desc(colon, colon + strlen(colon), &cls, &type, &uid))
        return FALSE;
    name = g_strndup(op + 1, cp - op - 1);
    owner = world_country_by_name(name);
    g_free(name);
    store_unit(cls, uid, owner, type, x, y, INT_UNKNOWN, INT_UNKNOWN);
    return TRUE;
}

/* "x,y is a N% <country> <sector type> with approximately M military."
 * (attack, assault, paradrop: what they are about to hit) */
static gboolean
try_attack(const char *line)
{
    const char *p = strstr(line, " is a "), *w, *pct;
    int x, y;
    char *end, *rest;
    SectIntel s;

    if (!p || !(w = strstr(p, " with approximately "))
        || !g_str_has_suffix(line, " military."))
        return FALSE;
    x = strtol(line, &end, 10);
    if (end == line || *end != ',')
        return FALSE;
    y = strtol(end + 1, &end, 10);
    if (end != p)
        return FALSE;
    pct = p + 6;
    sect_intel_init(&s);
    s.x = world_wrap_x(x);
    s.y = world_wrap_y(y);
    s.eff = strtol(pct, &end, 10);
    if (end == pct || *end != '%' || end[1] != ' ')
        return FALSE;
    s.mil = atoi(w + 20);
    /* "<country> <sector type>": the country name may have spaces, so
     * try each split until both halves are known */
    rest = g_strndup(end + 2, w - (end + 2));
    for (char *sp = strchr(rest, ' '); sp; sp = strchr(sp + 1, ' ')) {
        *sp = 0;
        s.owner = world_country_by_name(rest);
        s.des = world_des_by_name(sp + 1);
        *sp = ' ';
        if (s.owner >= 0 && s.des)
            break;
    }
    g_free(rest);
    if (s.owner < 0 || !s.des)
        return FALSE;
    store_sector(&s);
    return TRUE;
}

/* Satellite and spy plane sector rows:
 *    sect   type own  eff eff eff eff  civ  mil  shl  gun iron  pet  food
 *    3,-1      c    2  100   0   0   0  950   20    0    0    0    0   100 */
static gboolean
try_sat_sect(const char *line)
{
    int x, y, v[12];
    char *end, des;
    const char *p;
    SectIntel s;

    x = strtol(line, &end, 10);
    if (end == line || *end != ',')
        return FALSE;
    y = strtol(end + 1, &end, 10);
    p = end;
    while (*p == ' ')
        p++;
    if (!*p || p[1] != ' ' || g_ascii_isdigit(*p))
        return FALSE;
    des = *p;
    if (sscanf(p + 1, "%d %d %d %d %d %d %d %d %d %d %d %d", &v[0], &v[1],
               &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9],
               &v[10], &v[11]) != 12)
        return FALSE;
    sect_intel_init(&s);
    s.x = world_wrap_x(x);
    s.y = world_wrap_y(y);
    s.des = des;
    s.owner = v[0];
    s.eff = v[1];
    s.road = v[2];
    s.rail = v[3];
    s.defense = v[4];
    s.civ = v[5];
    s.mil = v[6];
    s.shell = v[7];
    s.gun = v[8];
    s.iron = v[9];
    s.petrol = v[10];
    s.food = v[11];
    if (s.owner)
        store_sector(&s);
    return TRUE;
}

/* Satellite and spy plane unit rows, after a heading saying which:
 *     own shp# ship type                                   sector   eff
 *       2    4 dd   destroyer  Bounty                    -3,3    100%
 *     own lnd# unit type         sector   eff
 *       2    3 inf  infantry    -2,4    100%
 * The type ("mnemonic  name") is cut to 16 characters; ships have their
 * own name after it. */
static gboolean
try_sat_unit(const char *line)
{
    const char *p = line, *tail;
    int owner, uid, type, x, y, eff;
    char *end, *name;
    gsize len = strlen(line);

    while (*p == '\t' || *p == ' ')
        p++;
    if (len < 20 || line[len - 1] != '%')
        return FALSE;
    owner = strtol(p, &end, 10);
    if (end == p || *end != ' ')
        return FALSE;
    p = end;
    uid = strtol(p, &end, 10);
    if (end == p || *end != ' ')
        return FALSE;
    p = end + 1;                /* start of the type name */
    /* from the right: "x,y    eff%" */
    tail = line + len - 1;
    while (tail > p && tail[-1] != ' ')
        tail--;
    eff = atoi(tail);
    while (tail > p && tail[-1] == ' ')
        tail--;
    while (tail > p && tail[-1] != ' ')
        tail--;
    x = strtol(tail, &end, 10);
    if (end == tail || *end != ',')
        return FALSE;
    y = strtol(end + 1, &end, 10);
    if (tail - p < 1)
        return FALSE;
    name = g_strndup(p, MIN(16, tail - p));
    if (strchr(name, ' '))
        *strchr(name, ' ') = 0;
    type = world_unit_type_by_mnem(in.sat_cls, name);
    g_free(name);
    if (type < 0)
        return FALSE;
    store_unit(in.sat_cls, uid, owner, type, world_wrap_x(x), world_wrap_y(y),
               INT_UNKNOWN, eff);
    return TRUE;
}

/* Have we seen owner's units of class cls here in the last minutes? */
static gboolean
seen_here(int x, int y, int cls, int owner)
{
    sqlite3_stmt *st;
    gboolean seen = FALSE;

    if (sqlite3_prepare_v2(db_handle(), "SELECT 1 FROM intel_unit WHERE x=? "
                           "AND y=? AND cls=? AND owner=? AND seen >= ?", -1,
                           &st, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_int(st, 1, x);
    sqlite3_bind_int(st, 2, y);
    sqlite3_bind_int(st, 3, cls);
    sqlite3_bind_int(st, 4, owner);
    sqlite3_bind_int64(st, 5, now() - 300);
    seen = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return seen;
}

/* "Flying over <country> ships in x,y" (reconnaissance without a spy
 * plane: there is no telling which ships) */
static gboolean
try_flying_over(const char *line)
{
    const char *p, *in_;
    int x, y, owner;
    gboolean ships;
    char *name, *end;

    if (!g_str_has_prefix(line, "Flying over "))
        return FALSE;
    p = line + 12;
    if ((in_ = strstr(p, " ships in ")))
        ships = TRUE;
    else if ((in_ = strstr(p, " land units in ")))
        ships = FALSE;
    else
        return FALSE;
    name = g_strndup(p, in_ - p);
    owner = world_country_by_name(name);
    g_free(name);
    p = in_ + (ships ? 10 : 15);
    x = strtol(p, &end, 10);
    if (end == p || *end != ',')
        return FALSE;
    y = strtol(end + 1, &end, 10);
    x = world_wrap_x(x);
    y = world_wrap_y(y);
    if ((owner >= 0 && owner == my_cnum)
        || seen_here(x, y, ships ? UC_SHIP : UC_LAND, owner))
        return TRUE;
    store_contact(x, y, ships ? 'S' : 'L', ships ? "ships" : "land units",
                  owner);
    return TRUE;
}

static gboolean
letter_is(UnitClass c, int type, char letter)
{
    const char *n = world_unit_type_name(c, type);

    return n[0] && g_ascii_toupper(n[0]) == letter;
}

/* A unit radar or a satellite image shows as letter (the first letter
 * of its type) that we know already: one of ours, or one seen in this
 * sector just now */
static gboolean
known_letter(int x, int y, char letter)
{
    sqlite3_stmt *st;
    gboolean known = FALSE;

    for (int c = UC_SHIP; c <= UC_LAND; c++) {
        GPtrArray *a = world_units(c);

        for (guint i = 0; i < a->len; i++) {
            Unit *u = g_ptr_array_index(a, i);

            if (u->x == x && u->y == y && letter_is(c, u->type, letter))
                return TRUE;
        }
    }
    if (sqlite3_prepare_v2(db_handle(), "SELECT cls, type FROM intel_unit "
                           "WHERE x=? AND y=? AND seen >= ?", -1, &st, NULL)
        != SQLITE_OK)
        return FALSE;
    sqlite3_bind_int(st, 1, x);
    sqlite3_bind_int(st, 2, y);
    sqlite3_bind_int64(st, 3, now() - 300);
    while (!known && sqlite3_step(st) == SQLITE_ROW)
        known = letter_is(sqlite3_column_int(st, 0),
                          sqlite3_column_int(st, 1), letter);
    sqlite3_finalize(st);
    return known;
}

/* "[<unit> at ]x,y efficiency N%, max range R" starts a radar map
 * (satellites print it too, and their map later, after "Satellite
 * radar report") */
static gboolean
try_radar_head(const char *line)
{
    const char *e = strstr(line, " efficiency "), *p;
    int x, y, eff, r;
    char *end;

    if (!e || !strstr(e, "%, max range "))
        return FALSE;
    for (p = e; p > line && p[-1] != ' '; p--)
        ;
    x = strtol(p, &end, 10);
    if (end == p || *end != ',')
        return FALSE;
    y = strtol(end + 1, &end, 10);
    if (end != e || sscanf(e, " efficiency %d%%, max range %d", &eff, &r) != 2)
        return FALSE;
    in.rad_x = x;
    in.rad_y = y;
    in.rad_range = r;
    in.rad_pending = TRUE;
    in.rad_wait = FALSE;
    in.rad_active = FALSE;
    in.rad_row = 0;
    return TRUE;
}

/* One row of a radar map.  Row r, column c is the sector
 * (cx - 2R + c, cy - R + r), or from -WORLD/2 when the radar sees the
 * whole world; blanks are not sectors.  Capital letters are ships (the
 * first letter of the type), $ satellites, 0 the radar itself. */
static void
radar_row(const char *line)
{
    int r = in.rad_range;
    int lx = 4 * r + 1 < world_x ? in.rad_x - 2 * r : in.rad_x - world_x / 2;
    int ly = 2 * r + 1 < world_y ? in.rad_y - r : in.rad_y - world_y / 2;
    int height = MIN(2 * r + 1, world_y);
    int y = world_wrap_y(ly + in.rad_row);

    for (int c = 0; line[c]; c++) {
        int x = world_wrap_x(lx + c);
        char ch = line[c];

        if (ch == ' ' || ch == '0')
            continue;
        if (g_ascii_isupper(ch)) {
            /* radars show ships; satellite images land units too */
            char *what = g_strdup_printf("%s, type starting %c",
                                         in.rad_wait ? "ship or land unit"
                                         : "ship", ch);

            if (known_letter(x, y, ch))
                forget_contact(x, y);
            else
                store_contact(x, y, ch, what, -1);
            g_free(what);
        } else if (ch == '$') {
            store_contact(x, y, '$', "satellite", -1);
        } else {
            forget_contact(x, y);
        }
    }
    if (++in.rad_row >= height)
        in.rad_active = FALSE;
}

void
intel_command_start(const char *cmd)
{
    in.source[0] = 0;
    sscanf(cmd, " %15[a-zA-Z]", in.source);
    in.in_spy = in.in_sat = FALSE;
    in.rad_pending = in.rad_wait = in.rad_active = FALSE;
    in.changed = FALSE;
}

void
intel_line(const char *text)
{
    int x, y;
    const char *at;

    if (!db_is_open() || world_x <= 0)
        return;
    /* radar maps */
    if (in.rad_active) {
        if (!*text)
            in.rad_active = FALSE;
        else
            radar_row(text);
        return;
    }
    if (try_radar_head(text))
        return;
    if (in.rad_pending) {
        if (!strcmp(text, "Satellite radar report")) {
            in.rad_pending = FALSE;
            in.rad_active = TRUE;
            return;
        }
        if (g_str_has_prefix(text, "Satellite ")
            || g_str_has_prefix(text, "Some noise")) {
            in.rad_wait = TRUE;         /* a satellite: the map comes later */
        } else if (!in.rad_wait) {
            in.rad_pending = FALSE;     /* a radar: this is the first row */
            in.rad_active = TRUE;
            radar_row(text);
            return;
        }
    }
    if (!strcmp(text, "SPY report")) {
        in.in_spy = TRUE;
        return;
    }
    if (in.in_spy && (try_spy_row(text) || try_spy_unit(text)))
        return;
    /* satellite and spy plane reports */
    if (!strcmp(text, "Satellite sector report")
        || !strcmp(text, "SPY Plane report")) {
        in.in_sat = TRUE;
        return;
    }
    if (strstr(text, " own shp# ")) {
        in.sat_cls = UC_SHIP;
        in.in_sat = TRUE;
        return;
    }
    if (strstr(text, " own lnd# ")) {
        in.sat_cls = UC_LAND;
        in.in_sat = TRUE;
        return;
    }
    if (in.in_sat && (try_sat_sect(text) || try_sat_unit(text)))
        return;
    if (try_flying_over(text) || try_attack(text))
        return;
    if (!strchr(text, '@') || !parse_at(text, &x, &y, &at))
        return;
    x = world_wrap_x(x);
    y = world_wrap_y(y);
    if (try_sonar(text, at, x, y))
        return;
    if (try_sector(text, at, x, y))
        return;
    try_unit(text, at, x, y);
}

void
intel_command_end(void)
{
    if (in.in_txn) {
        sqlite3_exec(db_handle(), "COMMIT", NULL, NULL, NULL);
        in.in_txn = FALSE;
    }
    in.in_spy = in.in_sat = FALSE;
    in.rad_pending = in.rad_wait = in.rad_active = FALSE;
    if (in.changed) {
        in.changed = FALSE;
        mainwin_data_changed();
    }
}

void
intel_clear(void)
{
    if (!ensure_tables())
        return;
    sqlite3_exec(db_handle(), "DELETE FROM intel_sect; DELETE FROM intel_unit; "
                 "DELETE FROM intel_contact;",
                 NULL, NULL, NULL);
    mainwin_data_changed();
}

char *
intel_age(gint64 seen)
{
    gint64 d = now() - seen;

    if (d < 60)
        return g_strdup("just now");
    if (d < 3600)
        return g_strdup_printf("%d min ago", (int)(d / 60));
    if (d < 2 * 86400)
        return g_strdup_printf("%d h ago", (int)(d / 3600));
    return g_strdup_printf("%d days ago", (int)(d / 86400));
}
