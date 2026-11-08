/*
 * main.c - GtkACE start-up.
 */
#include <string.h>
#include "gtkace.h"

static char *start_game;        /* game named on the command line */
static gboolean start_offline;

static Game *
find_game(const char *name)
{
    GList *games = games_load();
    Game *found = NULL;

    for (GList *l = games; l; l = l->next)
        if (!strcmp(((Game *)l->data)->name, name))
            found = game_copy(l->data);
    games_free(games);
    return found;
}

static gboolean
choose_game(gpointer data)
{
    gboolean connect = FALSE;
    Game *g = NULL;

    if (start_game) {
        g = find_game(start_game);
        connect = !start_offline;
        if (!g) {
            char *m = g_strdup_printf("There is no saved game called \"%s\".",
                                      start_game);
            mainwin_error("Unknown game", m);
            g_free(m);
        }
        g_clear_pointer(&start_game, g_free);
    }
    if (!g)
        g = game_choose(mainwin_window(), &connect);

    if (g) {
        mainwin_set_game(g, connect);
        game_free(g);
    } else {
        mapview_redraw();
    }
    return G_SOURCE_REMOVE;
}

static void
activate(GtkApplication *app, gpointer data)
{
    if (mainwin_window()) {
        gtk_window_present(mainwin_window());
        return;
    }
    scheme_defaults();
    mainwin_new(app);
    out_text(GTKACE_NAME " " GTKACE_VERSION
             " - a client for Wolfpack Empire, based on WinACE.", "notice");
    /* let the main window appear before the modal chooser */
    g_idle_add(choose_game, NULL);
}

int
main(int argc, char **argv)
{
    GtkApplication *app;
    int status, i, n = 1;

    /* gtkace [--offline] [GAME] */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--offline"))
            start_offline = TRUE;
        else if (!strcmp(argv[i], "--version")) {
            g_print("%s %s\n", GTKACE_NAME, GTKACE_VERSION);
            return 0;
        } else if (!strcmp(argv[i], "--help") || argv[i][0] == '-') {
            g_print("Usage: %s [--offline] [GAME]\n"
                    "Opens GAME (a name from the game list) and connects,\n"
                    "or shows the game list if no game is given.\n", argv[0]);
            return argv[i][1] == '-' && !strcmp(argv[i], "--help") ? 0 : 1;
        } else {
            g_free(start_game);
            start_game = g_strdup(argv[i]);
        }
    }
    argv[n] = NULL;             /* the rest is ours, not GApplication's */

    g_set_application_name(GTKACE_NAME);
    app = gtk_application_new("io.github.fstltna.GtkACE",
                              G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), n, argv);
    g_object_unref(app);
    return status;
}
