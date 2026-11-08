/*
 * db.c - the per-game database (WinACE EmpDataBase/XdumpParse).
 *
 * WinACE kept its data in a Microsoft Access database.  GtkACE uses
 * SQLite and creates its tables straight from the server's own table
 * descriptions ("xdump meta"), so new server versions with extra
 * columns need no client changes.
 */
#include <string.h>
#include <stdlib.h>
#include <sqlite3.h>
#include "gtkace.h"

static sqlite3 *db;

void *
db_handle(void)
{
    return db;
}

gboolean
db_is_open(void)
{
    return db != NULL;
}

static gboolean
exec(const char *sql)
{
    char *err = NULL;

    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        g_warning("SQL error: %s\n  in: %.200s", err, sql);
        sqlite3_free(err);
        return FALSE;
    }
    return TRUE;
}

gboolean
db_open(const char *path)
{
    db_close();
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        g_warning("Cannot open %s: %s", path, sqlite3_errmsg(db));
        sqlite3_close(db);
        db = NULL;
        return FALSE;
    }
    sqlite3_busy_timeout(db, 2000);
    exec("PRAGMA journal_mode=WAL");
    exec("CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value)");
    exec("CREATE TABLE IF NOT EXISTS xmeta (tbl TEXT, idx INTEGER, "
         "name TEXT, type INTEGER, flags INTEGER, len INTEGER, "
         "symtab INTEGER, PRIMARY KEY (tbl, idx))");
    exec("CREATE TABLE IF NOT EXISTS bmap (x INTEGER, y INTEGER, des TEXT, "
         "PRIMARY KEY (x, y))");
    return TRUE;
}

void
db_close(void)
{
    if (db) {
        sqlite3_close(db);
        db = NULL;
    }
}

gboolean
db_table_exists(const char *table)
{
    sqlite3_stmt *st;
    gboolean found = FALSE;

    if (!db)
        return FALSE;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table'"
                           " AND name=?", -1, &st, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);
    found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

int
db_get_int(const char *sql, int dflt)
{
    sqlite3_stmt *st;
    int v = dflt;

    if (!db || sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return dflt;
    if (sqlite3_step(st) == SQLITE_ROW
        && sqlite3_column_type(st, 0) != SQLITE_NULL)
        v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

char *
db_get_text(const char *sql)
{
    sqlite3_stmt *st;
    char *v = NULL;

    if (!db || sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return NULL;
    if (sqlite3_step(st) == SQLITE_ROW
        && sqlite3_column_type(st, 0) != SQLITE_NULL)
        v = g_strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return v;
}

void
db_set_setting(const char *key, const char *value)
{
    sqlite3_stmt *st;

    if (!db || sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO settings "
                                  "VALUES (?, ?)", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

char *
db_get_setting(const char *key)
{
    sqlite3_stmt *st;
    char *v = NULL;

    if (!db || sqlite3_prepare_v2(db, "SELECT value FROM settings WHERE key=?",
                                  -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW
        && sqlite3_column_type(st, 0) != SQLITE_NULL)
        v = g_strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return v;
}

/* ------------------------------------------------------------ xdump */

GPtrArray *
xdump_split(const char *line)
{
    GPtrArray *tok = g_ptr_array_new_with_free_func(g_free);
    const char *p = line;

    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        if (*p == '"') {
            GString *s = g_string_new(NULL);

            for (p++; *p && *p != '"'; p++) {
                if (*p == '\\' && p[1] >= '0' && p[1] <= '7'
                    && p[2] >= '0' && p[2] <= '7' && p[3] >= '0' && p[3] <= '7') {
                    g_string_append_c(s, (p[1] - '0') * 64 + (p[2] - '0') * 8
                                      + (p[3] - '0'));
                    p += 3;
                } else {
                    g_string_append_c(s, *p);
                }
            }
            if (*p != '"') {    /* unterminated */
                g_string_free(s, TRUE);
                g_ptr_array_unref(tok);
                return NULL;
            }
            p++;
            g_ptr_array_add(tok, g_string_free(s, FALSE));
        } else {
            const char *start = p;

            while (*p && *p != ' ')
                p++;
            if (p - start == 3 && !strncmp(start, "nil", 3))
                g_ptr_array_add(tok, NULL);
            else
                g_ptr_array_add(tok, g_strndup(start, p - start));
        }
    }
    return tok;
}

typedef struct {
    char *table;
    GPtrArray *meta;        /* GPtrArray* tokens per meta row */
    GPtrArray *rows;        /* GPtrArray* tokens per data row */
    GPtrArray *cur;         /* where lines go now */
    gboolean in_dump;       /* between XDUMP header and /n footer */
    gboolean meta_ok, rows_ok;
    CmdDoneFunc done;
    gpointer data;
} Fetch;

static void
fetch_free(Fetch *f)
{
    g_free(f->table);
    g_ptr_array_unref(f->meta);
    g_ptr_array_unref(f->rows);
    g_free(f);
}

static void
fetch_line(int id, const char *text, gpointer data)
{
    Fetch *f = data;
    GPtrArray *tok;

    if (id != C_DATA)
        return;
    if (!f->in_dump) {
        if (!strncmp(text, "XDUMP ", 6))
            f->in_dump = TRUE;
        return;                 /* also skips error messages */
    }
    if (text[0] == '/') {
        f->in_dump = FALSE;
        if (atoi(text + 1) == (int)f->cur->len) {
            if (f->cur == f->meta)
                f->meta_ok = TRUE;
            else
                f->rows_ok = TRUE;
        } else {
            g_warning("xdump %s: expected %s rows, got %u", f->table, text + 1,
                      f->cur->len);
        }
        return;
    }
    tok = xdump_split(text);
    if (tok)
        g_ptr_array_add(f->cur, tok);
}

static char *quote_ident(const char *s);

/* Column names for a table description.  A field with a length
 * (an array, such as the relations of a country to all others) has
 * one column per element: relations0, relations1, ... */
static GPtrArray *
meta_columns(GPtrArray *meta)
{
    GPtrArray *cols = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < meta->len; i++) {
        GPtrArray *m = g_ptr_array_index(meta, i);
        const char *name = m->len && m->pdata[0] ? m->pdata[0] : "?";
        int len = m->len > 3 && m->pdata[3] ? atoi(m->pdata[3]) : 0;

        if (len > 0 && len <= 4096)
            for (int j = 0; j < len; j++)
                g_ptr_array_add(cols, g_strdup_printf("%s%d", name, j));
        else
            g_ptr_array_add(cols, g_strdup(name));
    }
    return cols;
}

/* Does the stored table have exactly these columns? */
static gboolean
table_matches(const char *table, GPtrArray *cols)
{
    sqlite3_stmt *st;
    char *qt, *sql;
    guint n = 0;
    gboolean same = TRUE;

    if (!db_table_exists(table))
        return FALSE;
    qt = quote_ident(table);
    sql = g_strdup_printf("PRAGMA table_info(%s)", qt);
    g_free(qt);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        g_free(sql);
        return FALSE;
    }
    g_free(sql);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 1);

        if (n >= cols->len || !name || strcmp(name, cols->pdata[n]))
            same = FALSE;
        n++;
    }
    sqlite3_finalize(st);
    return same && n == cols->len;
}

static char *
quote_ident(const char *s)
{
    GString *q = g_string_new("\"");

    for (; *s; s++) {
        if (*s == '"')
            g_string_append_c(q, '"');
        g_string_append_c(q, *s);
    }
    g_string_append_c(q, '"');
    return g_string_free(q, FALSE);
}

static void
bind_value(sqlite3_stmt *st, int col, const char *v)
{
    char *end;

    if (!v) {
        sqlite3_bind_null(st, col);
        return;
    }
    if (*v) {
        long long i = strtoll(v, &end, 10);

        if (!*end) {
            sqlite3_bind_int64(st, col, i);
            return;
        }
        double d = g_ascii_strtod(v, &end);
        if (!*end) {
            sqlite3_bind_double(st, col, d);
            return;
        }
    }
    sqlite3_bind_text(st, col, v, -1, SQLITE_TRANSIENT);
}

static gboolean
store_table(Fetch *f)
{
    char *qt = quote_ident(f->table), *sql;
    GPtrArray *cols = meta_columns(f->meta);
    GString *s;
    sqlite3_stmt *st;
    guint i, j, ncols = cols->len;
    int skipped = 0;

    if (!ncols) {
        g_ptr_array_unref(cols);
        g_free(qt);
        return FALSE;
    }
    exec("BEGIN");
    if (!table_matches(f->table, cols)) {
        sql = g_strdup_printf("DROP TABLE IF EXISTS %s", qt);
        exec(sql);
        g_free(sql);
        s = g_string_new(NULL);
        g_string_printf(s, "CREATE TABLE %s (", qt);
        for (i = 0; i < ncols; i++) {
            char *qc = quote_ident(cols->pdata[i]);

            g_string_append_printf(s, "%s%s", i ? ", " : "", qc);
            g_free(qc);
        }
        g_string_append_c(s, ')');
        if (!exec(s->str)) {
            g_string_free(s, TRUE);
            exec("ROLLBACK");
            g_ptr_array_unref(cols);
            g_free(qt);
            return FALSE;
        }
        g_string_free(s, TRUE);
    }

    /* remember the description for offline use and later sessions */
    sqlite3_prepare_v2(db, "DELETE FROM xmeta WHERE tbl=?", -1, &st, NULL);
    sqlite3_bind_text(st, 1, f->table, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "INSERT INTO xmeta VALUES (?,?,?,?,?,?,?)", -1,
                       &st, NULL);
    for (i = 0; i < f->meta->len; i++) {
        GPtrArray *m = g_ptr_array_index(f->meta, i);

        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, f->table, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, i);
        for (j = 0; j < 5; j++)
            bind_value(st, 3 + j, j < m->len ? m->pdata[j] : NULL);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    sql = g_strdup_printf("DELETE FROM %s", qt);
    exec(sql);
    g_free(sql);

    s = g_string_new(NULL);
    g_string_printf(s, "INSERT INTO %s VALUES (", qt);
    for (i = 0; i < ncols; i++)
        g_string_append(s, i ? ",?" : "?");
    g_string_append_c(s, ')');
    if (sqlite3_prepare_v2(db, s->str, -1, &st, NULL) != SQLITE_OK) {
        g_warning("SQL error: %s", sqlite3_errmsg(db));
        g_string_free(s, TRUE);
        exec("ROLLBACK");
        g_ptr_array_unref(cols);
        g_free(qt);
        return FALSE;
    }
    g_string_free(s, TRUE);
    for (i = 0; i < f->rows->len; i++) {
        GPtrArray *r = g_ptr_array_index(f->rows, i);

        if (r->len != ncols) {
            skipped++;
            continue;
        }
        sqlite3_reset(st);
        for (j = 0; j < ncols; j++)
            bind_value(st, j + 1, r->pdata[j]);
        if (sqlite3_step(st) != SQLITE_DONE)
            g_warning("Insert into %s failed: %s", f->table,
                      sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
    exec("COMMIT");
    if (skipped)
        g_warning("xdump %s: %d rows did not match the %u columns", f->table,
                  skipped, ncols);
    g_ptr_array_unref(cols);
    g_free(qt);
    return TRUE;
}

/* Tables whose description was fetched in this login session; a
 * server's tables cannot change while it runs. */
static GHashTable *meta_known;

void
db_new_session(void)
{
    if (meta_known)
        g_hash_table_remove_all(meta_known);
}

static gboolean
load_stored_meta(Fetch *f)
{
    sqlite3_stmt *st;

    if (!db || !meta_known || !g_hash_table_contains(meta_known, f->table)
        || !db_table_exists(f->table))
        return FALSE;
    if (sqlite3_prepare_v2(db, "SELECT name, type, flags, len, symtab FROM "
                           "xmeta WHERE tbl=? ORDER BY idx", -1, &st,
                           NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_text(st, 1, f->table, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        GPtrArray *m = g_ptr_array_new_with_free_func(g_free);

        for (int j = 0; j < 5; j++)
            g_ptr_array_add(m, g_strdup((const char *)sqlite3_column_text(st, j)));
        g_ptr_array_add(f->meta, m);
    }
    sqlite3_finalize(st);
    return f->meta->len > 0;
}

static void
fetch_meta_done(gpointer data)
{
    Fetch *f = data;

    f->cur = f->rows;
    f->in_dump = FALSE;
}

static void
fetch_rows_done(gpointer data)
{
    Fetch *f = data;

    if (db && f->meta_ok && f->rows_ok && store_table(f)) {
        if (!meta_known)
            meta_known = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
        g_hash_table_add(meta_known, g_strdup(f->table));
    }
    if (f->done)
        f->done(f->data);
    fetch_free(f);
}

static void
fetch_meta_only_done(gpointer data)
{
    /* the connection went away before the data request was queued */
    fetch_rows_done(data);
}

void
db_fetch_table(const char *table, CmdDoneFunc done, gpointer data)
{
    Fetch *f = g_new0(Fetch, 1);
    char *cmd;

    f->table = g_strdup(table);
    f->meta = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
    f->rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
    f->cur = f->meta;
    f->done = done;
    f->data = data;
    if (conn_state() != CONN_PLAYING) {
        fetch_meta_only_done(f);
        return;
    }
    if (load_stored_meta(f)) {
        f->meta_ok = TRUE;
        f->cur = f->rows;
    } else {
        g_ptr_array_set_size(f->meta, 0);
        /* Both commands are queued now, so they run back to back */
        cmd = g_strdup_printf("xdump meta %s", table);
        conn_command(cmd, 0, fetch_line, fetch_meta_done, f);
        g_free(cmd);
    }
    cmd = g_strdup_printf("xdump %s *", table);
    conn_command(cmd, 0, fetch_line, fetch_rows_done, f);
    g_free(cmd);
}

/* ------------------------------------------------------------- maps */

/* Map output looks like this (bmap of a 32x16 world):
 *
 *          1111111---------0000000000111111
 *          65432109876543210123456789012345
 *       -8                                  -8
 *       -7     . . - ^                      -7
 *       ...
 *          1111111---------0000000000111111
 *          65432109876543210123456789012345
 *
 * The x coordinate of a column is written vertically in the header.
 * Negative numbers lose their sign when they have as many digits as
 * the header has lines, so the columns are numbered from an anchor
 * that can be read unambiguously: one whose top line is '-' or '0'. */
gboolean
map_parse(GPtrArray *lines, void (*fn)(int x, int y, char des, gpointer),
          gpointer data)
{
    GPtrArray *head = g_ptr_array_new();
    guint i, first_row = 0, ncols = 0;
    int start = -1, anchor_col = -1, anchor_x = 0;
    gboolean found_row = FALSE;

    for (i = 0; i < lines->len; i++) {
        const char *l = g_ptr_array_index(lines, i);
        const char *p = l;

        while (*p == ' ')
            p++;
        if (!*p)
            continue;
        if (p[strspn(p, "-0123456789")] == '\0') {
            /* a header line is a run of digits and '-' only */
            g_ptr_array_add(head, (gpointer)l);
        } else if (head->len) {
            /* a row starts with its y coordinate followed by a space */
            const char *q = p + (*p == '-');

            if (!g_ascii_isdigit(*q))
                break;
            while (g_ascii_isdigit(*q))
                q++;
            if (*q == ' ') {
                first_row = i;
                found_row = TRUE;
            }
            break;
        }
    }
    if (!found_row || head->len < 1) {
        g_ptr_array_free(head, TRUE);
        return FALSE;
    }
    /* the header lines all start in the same column */
    {
        const char *h = g_ptr_array_index(head, 0);

        start = strspn(h, " ");
        ncols = strlen(h + start);
        for (i = 1; i < head->len; i++) {
            const char *hi = g_ptr_array_index(head, i);

            if ((int)strspn(hi, " ") != start || strlen(hi + start) != ncols) {
                g_ptr_array_free(head, TRUE);
                return FALSE;
            }
        }
    }
    for (guint c = 0; c < ncols && anchor_col < 0; c++) {
        const char *top = g_ptr_array_index(head, 0);
        char t = top[start + c];
        int v = 0;
        gboolean ok = TRUE;

        if (t != '-' && t != '0')
            continue;
        for (i = (t == '-'); i < head->len; i++) {
            char d = ((const char *)g_ptr_array_index(head, i))[start + c];

            if (!g_ascii_isdigit(d)) {
                ok = FALSE;
                break;
            }
            v = v * 10 + (d - '0');
        }
        if (ok) {
            anchor_col = c;
            anchor_x = t == '-' ? -v : v;
        }
    }
    g_ptr_array_free(head, TRUE);
    if (anchor_col < 0)
        return FALSE;

    for (i = first_row; i < lines->len; i++) {
        const char *l = g_ptr_array_index(lines, i);
        const char *p = l + strspn(l, " ");
        char *end;
        long y;

        if (!*p || p[strspn(p, "-0123456789")] == '\0')
            continue;           /* blank or trailing header line */
        y = strtol(l, &end, 10);
        if (end == l || *end != ' ' || (int)(end - l) > start)
            continue;
        for (guint c = 0; c < ncols && start + c < strlen(l); c++) {
            char ch = l[start + c] & 0x7f;
            int x = anchor_x + ((int)c - anchor_col);

            if (ch == ' ')
                continue;
            if (world_x > 0)
                x = world_wrap_x(x);
            fn(x, world_y > 0 ? world_wrap_y(y) : (int)y, ch, data);
        }
    }
    return TRUE;
}

typedef struct {
    GPtrArray *lines;
    CmdDoneFunc done;
    gpointer data;
    sqlite3_stmt *st;
    int count;
} BmapFetch;

static void
bmap_line(int id, const char *text, gpointer data)
{
    BmapFetch *b = data;

    if (id == C_DATA)
        g_ptr_array_add(b->lines, g_strdup(text));
}

static void
bmap_store(int x, int y, char des, gpointer data)
{
    BmapFetch *b = data;
    char s[2] = { des, 0 };

    sqlite3_reset(b->st);
    sqlite3_bind_int(b->st, 1, x);
    sqlite3_bind_int(b->st, 2, y);
    sqlite3_bind_text(b->st, 3, s, 1, SQLITE_TRANSIENT);
    sqlite3_step(b->st);
    b->count++;
}

static void
bmap_done(gpointer data)
{
    BmapFetch *b = data;

    if (db && b->lines->len) {
        exec("BEGIN");
        if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO bmap VALUES (?,?,?)",
                               -1, &b->st, NULL) == SQLITE_OK) {
            if (!map_parse(b->lines, bmap_store, b))
                g_warning("Could not understand the bmap output");
            sqlite3_finalize(b->st);
        }
        exec("COMMIT");
    }
    if (b->done)
        b->done(b->data);
    g_ptr_array_unref(b->lines);
    g_free(b);
}

void
db_fetch_bmap(CmdDoneFunc done, gpointer data)
{
    BmapFetch *b = g_new0(BmapFetch, 1);

    b->lines = g_ptr_array_new_with_free_func(g_free);
    b->done = done;
    b->data = data;
    conn_command("bmap *", 0, bmap_line, bmap_done, b);
}
