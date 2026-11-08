/*
 * GtkACE - a GTK 3 client for Wolfpack Empire.
 *
 * Ported from WinACE 2.5.72 (Windows Advanced Client for Empire),
 * which is distributed under the Artistic License; see LICENSE and
 * CREDITS.  GtkACE is a separate, renamed program and is not WinACE.
 */
#ifndef GTKACE_H
#define GTKACE_H

#include <gtk/gtk.h>
#include "version.h"

/* ------------------------------------------------------------ games */

typedef struct {
    char *name;          /* unique game name, also names the database */
    char *host;
    int   port;
    char *country;
    char *password;
    char *user;          /* login user name (informational) */
    gboolean kill;       /* kill a hung session for this country */
    gboolean deity;
    gboolean autoupdate; /* refresh the database after logging in */
    gboolean autoread;   /* read telegrams when they arrive */
    gboolean utf8;       /* ask the server for UTF-8 */
} Game;

Game   *game_new(void);
Game   *game_copy(const Game *g);
void    game_free(Game *g);
GList  *games_load(void);                 /* list of Game*, MRU first */
void    games_save(GList *games);
void    games_free(GList *games);
void    games_touch(const char *name);    /* move to front of MRU list */
char   *game_db_path(const Game *g);      /* newly allocated */

/* Game chooser: returns the chosen game (caller frees) and whether
 * to connect (TRUE) or open it offline (FALSE).  NULL = cancelled. */
Game   *game_choose(GtkWindow *parent, gboolean *connect);

/* ------------------------------------------------------- connection */

enum {                   /* server output ids, see doc/clients-howto */
    C_CMDOK, C_DATA, C_INIT, C_EXIT, C_FLUSH, C_NOECHO, C_PROMPT,
    C_ABORT, C_REDIR, C_PIPE, C_CMDERR, C_BADCMD, C_EXECUTE, C_FLASH,
    C_INFORM
};

typedef enum {
    CONN_DOWN, CONN_CONNECTING, CONN_LOGIN, CONN_PLAYING
} ConnState;

/* Command flags */
#define CMD_SHOW   0x01  /* echo the command and show its output */
#define CMD_USER   0x02  /* typed by the user (goes into history) */

typedef void (*CmdLineFunc)(int id, const char *text, gpointer data);
typedef void (*CmdDoneFunc)(gpointer data);

void       conn_start(const Game *g);
void       conn_close(const char *why);
ConnState  conn_state(void);
gboolean   conn_busy(void);              /* a command is running */
gboolean   conn_wants_input(void);       /* server asked a question */
const char *conn_question(void);
void       conn_answer(const char *text);
void       conn_abort(void);
void       conn_command(const char *cmd, guint flags,
                        CmdLineFunc line, CmdDoneFunc done, gpointer data);
int        conn_queue_length(void);

/* ---------------------------------------------------------- database */

gboolean   db_open(const char *path);
void       db_close(void);
gboolean   db_is_open(void);
void      *db_handle(void);              /* sqlite3 * */

/* Fetch "xdump meta TABLE" then "xdump TABLE *" into the database.
 * done is called when the data has been stored (or failed). */
void       db_fetch_table(const char *table, CmdDoneFunc done, gpointer d);
void       db_fetch_bmap(CmdDoneFunc done, gpointer data);

gboolean   db_table_exists(const char *table);
int        db_get_int(const char *sql, int dflt);    /* first column */
char      *db_get_text(const char *sql);             /* newly allocated */
void       db_set_setting(const char *key, const char *value);
char      *db_get_setting(const char *key);

/* xdump helpers (exposed for testing).  Splits one xdump row into
 * decoded tokens; an element is NULL for nil.  NULL on a bad row. */
GPtrArray *xdump_split(const char *line);
/* Parses map-style output ("map", "bmap") lines.  Calls fn for every
 * sector character shown; returns FALSE if the lines are not a map. */
gboolean   map_parse(GPtrArray *lines,
                     void (*fn)(int x, int y, char des, gpointer), gpointer);

/* -------------------------------------------------------------- world */

/* Unit classes, in the order WinACE draws them */
typedef enum { UC_SHIP, UC_LAND, UC_PLANE, UC_NUKE, UC_COUNT } UnitClass;

typedef struct {
    char  des;           /* designation mnemonic, 0 = never seen */
    char  newdes;        /* designation being built, 0 = none */
    short own;           /* owner country number, -1 = unknown */
    guchar eff;
    guchar units[UC_COUNT];   /* our units here, saturating at 255 */
} Sector;

typedef struct {
    UnitClass cls;
    int uid, owner, x, y, type, eff, mob, tech;
    char group;          /* fleet, army or wing letter, 0 = none */
    int carrier;         /* unit carrying it, -1 = none */
    char carrier_cls;    /* 's'hip, 'l'and unit or 'p'lane */
} Unit;

extern int world_x, world_y;    /* world size, 0 = unknown */
extern int my_cnum;             /* our country number, -1 = unknown */

void       world_load(void);              /* rebuild from the database */
Sector    *world_sector(int x, int y);    /* NULL if off-world/invalid */
int        world_wrap_x(int x);
int        world_wrap_y(int y);
const char *world_country_name(int cnum); /* "" if unknown */
const char *world_des_name(char mnem);    /* "" if unknown */
int        world_sector_count(void);      /* sectors we own */
const char *world_unit_table(UnitClass c);  /* "ship", "land", ... */
const char *world_unit_noun(UnitClass c, gboolean plural);
/* our units, in uid order; the array is owned by world.c */
GPtrArray  *world_units(UnitClass c);
const char *world_unit_mnem(UnitClass c, int type);   /* "dd" */
const char *world_unit_type_name(UnitClass c, int type); /* "destroyer" */
char       *world_unit_describe(const Unit *u);       /* newly allocated */

/* --------------------------------------------------------------- map */

GtkWidget *mapview_new(void);
void       mapview_redraw(void);
void       mapview_center(int x, int y);
void       mapview_zoom(double factor);
void       mapview_select(int x, int y);
gboolean   mapview_selected(int *x, int *y);

/* ------------------------------------------------------ units window */

void       units_window_show(UnitClass c);
void       units_window_refresh(void);   /* data changed */

/* --------------------------------------------------------- view options */

extern gboolean show_units[UC_COUNT];   /* View menu toggles */

/* ------------------------------------------------------- main window */

GtkWidget *mainwin_new(GtkApplication *app);
GtkWindow *mainwin_window(void);
void       mainwin_set_game(const Game *g, gboolean connect);

void       out_text(const char *text, const char *tag);  /* + newline */
void       out_server_line(int id, const char *text, gboolean utf8);
void       out_prompt(const char *prompt);
void       status_set_conn(ConnState st, const char *detail);
void       status_set_btu(int minutes, int btu);
void       status_set_mail(const char *text);
void       status_set_busy(int queued);
void       status_set_question(const char *q);
void       status_hover(int x, int y, gboolean valid);
void       panel_show_sector(int x, int y);
void       mainwin_data_changed(void);   /* database updated */
void       mainwin_logged_in(void);
void       mainwin_disconnected(void);
void       mainwin_error(const char *title, const char *msg);

/* Refresh helpers (mainwin.c) */
void       refresh_all(void);
void       refresh_sectors(void);
void       refresh_bmap(void);
void       refresh_units(void);
void       refresh_after_command(const char *cmd); /* if it changed things */
void       db_new_session(void);          /* forget per-login caches */

/* Convert server text to valid UTF-8, marking highlighted runs.
 * Returns the text; *hl gets a newly allocated array of byte offsets
 * pairs (start,end) terminated by -1, or NULL if nothing highlighted. */
char      *emp_text_decode(const char *raw, gboolean utf8, int **hl);

#endif
