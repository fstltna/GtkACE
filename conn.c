/*
 * conn.c - connection to the Empire server: asynchronous connect,
 * the login dialogue, and the command queue for the playing phase
 * (WinACE frmEmpCmd).  The protocol is described in the server's
 * doc/clients-howto.
 *
 * GtkACE is a synchronous client: it sends a command only after the
 * server's command prompt (C_PROMPT), and a line of input for a command
 * only after an argument prompt (C_FLUSH).
 */
#include <string.h>
#include <stdlib.h>
#include <gio/gio.h>
#include "gtkace.h"

#define CONNECT_TIMEOUT 20      /* seconds */

typedef enum {
    LS_NONE, LS_INIT, LS_CLIENT, LS_OPTIONS, LS_USER, LS_COUN, LS_PASS,
    LS_KILL, LS_PLAY, LS_PLAYING
} LoginState;

typedef struct {
    char *text;
    guint flags;
    CmdLineFunc line;
    CmdDoneFunc done;
    gpointer data;
    GQueue answers;             /* char *: replies to the server's prompts */
    CmdAnswerFunc responder;    /* or a function that chooses replies */
} EmpCmd;

static struct {
    ConnState state;
    LoginState ls;
    Game *game;
    GSocketClient *client;
    GSocketConnection *conn;
    GCancellable *cancel;
    GString *inbuf;
    char rbuf[8192];
    gboolean utf8;           /* session is in UTF-8 */
    GQueue queue;            /* EmpCmd * waiting for a prompt */
    EmpCmd *current;         /* command the server is working on */
    gboolean prompted;       /* server is waiting for a command */
    char *question;          /* pending argument prompt, or NULL */
    char *login_error;       /* why the login failed */
    guint generation;        /* invalidates stale async callbacks */
} cs;

static void process_line(const char *line);
static void start_read(void);
static gboolean is_read_cmd(const char *cmd);

ConnState
conn_state(void)
{
    return cs.state;
}

gboolean
conn_busy(void)
{
    return cs.current != NULL || !cs.prompted;
}

gboolean
conn_wants_input(void)
{
    return cs.question != NULL;
}

const char *
conn_question(void)
{
    return cs.question;
}

int
conn_queue_length(void)
{
    return g_queue_get_length(&cs.queue) + (cs.current ? 1 : 0);
}

static void
cmd_free(EmpCmd *c)
{
    if (c) {
        g_queue_clear_full(&c->answers, g_free);
        g_free(c->text);
        g_free(c);
    }
}

static void
set_state(ConnState st, const char *detail)
{
    cs.state = st;
    status_set_conn(st, detail);
}

static void
send_line(const char *line)
{
    GOutputStream *out;
    GError *err = NULL;
    char *buf;

    if (!cs.conn)
        return;
    out = g_io_stream_get_output_stream(G_IO_STREAM(cs.conn));
    buf = g_strconcat(line, "\n", NULL);
    if (!g_output_stream_write_all(out, buf, strlen(buf), NULL, NULL, &err)) {
        char *why = g_strdup_printf("Write error: %s", err->message);

        g_error_free(err);
        g_free(buf);
        conn_close(why);
        g_free(why);
        return;
    }
    g_free(buf);
}

/* Login command with one quoted argument.  The server splits arguments
 * at spaces unless quoted, and has no way to escape a quote. */
static void
send_login(const char *cmd, const char *arg)
{
    GString *s = g_string_new(cmd);

    if (arg) {
        g_string_append(s, " \"");
        for (const char *p = arg; *p; p++)
            if (*p != '"' && *p != '\n' && *p != '\r')
                g_string_append_c(s, *p);
        g_string_append_c(s, '"');
    }
    send_line(s->str);
    g_string_free(s, TRUE);
}

static void
flush_queue(void)
{
    EmpCmd *c;

    /* run the done callbacks so callers waiting on them can clean up */
    if (cs.current) {
        c = cs.current;
        cs.current = NULL;
        if (c->done)
            c->done(c->data);
        cmd_free(c);
    }
    while ((c = g_queue_pop_head(&cs.queue))) {
        if (c->done)
            c->done(c->data);
        cmd_free(c);
    }
}

void
conn_close(const char *why)
{
    ConnState old = cs.state;

    if (old == CONN_DOWN)
        return;
    cs.generation++;
    if (cs.cancel) {
        g_cancellable_cancel(cs.cancel);
        g_clear_object(&cs.cancel);
    }
    if (cs.conn) {
        g_io_stream_close(G_IO_STREAM(cs.conn), NULL, NULL);
        g_clear_object(&cs.conn);
    }
    g_clear_object(&cs.client);
    cs.ls = LS_NONE;
    cs.prompted = FALSE;
    g_clear_pointer(&cs.question, g_free);
    status_set_question(NULL);
    set_state(CONN_DOWN, why);
    intel_command_end();
    tele_command_end();
    flush_queue();
    status_set_busy(0);
    if (why && *why)
        out_text(why, "notice");
    if (old == CONN_LOGIN || old == CONN_CONNECTING) {
        mainwin_error(old == CONN_CONNECTING ? "Could not connect"
                      : "Could not log in",
                      cs.login_error ? cs.login_error
                      : why ? why : "The connection was closed.");
    }
    g_clear_pointer(&cs.login_error, g_free);
    mainwin_disconnected();
}

/* ------------------------------------------------------- connecting */

static void
connected(GObject *src, GAsyncResult *res, gpointer gen)
{
    GError *err = NULL;
    GSocketConnection *conn;
    GSocket *sock;

    conn = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(src), res,
                                                  &err);
    if (GPOINTER_TO_UINT(gen) != cs.generation) {
        /* closed or restarted meanwhile */
        if (conn)
            g_object_unref(conn);
        if (err)
            g_error_free(err);
        return;
    }
    if (!conn) {
        char *why;

        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
            why = g_strdup_printf("Connecting to %s port %d timed out.",
                                  cs.game->host, cs.game->port);
        else
            why = g_strdup_printf("Unable to connect to %s port %d:\n%s",
                                  cs.game->host, cs.game->port, err->message);
        g_error_free(err);
        conn_close(why);
        g_free(why);
        return;
    }
    cs.conn = conn;
    sock = g_socket_connection_get_socket(conn);
    g_socket_set_keepalive(sock, TRUE);
    /* the timeout was for connecting; the session may idle much longer */
    g_socket_set_timeout(sock, 0);
    cs.ls = LS_INIT;
    set_state(CONN_LOGIN, "Logging in");
    start_read();
}

void
conn_start(const Game *g)
{
    if (cs.state != CONN_DOWN)
        conn_close(NULL);
    game_free(cs.game);
    cs.game = game_copy(g);
    cs.utf8 = FALSE;
    if (!cs.inbuf)
        cs.inbuf = g_string_new(NULL);
    g_string_truncate(cs.inbuf, 0);
    cs.generation++;
    cs.cancel = g_cancellable_new();
    cs.client = g_socket_client_new();
    /* Empire speaks plain TCP; a desktop proxy setting must not apply */
    g_socket_client_set_enable_proxy(cs.client, FALSE);
    g_socket_client_set_timeout(cs.client, CONNECT_TIMEOUT);
    {
        char *msg = g_strdup_printf("Connecting to %s port %d...",
                                    g->host, g->port);
        set_state(CONN_CONNECTING, msg);
        out_text(msg, "notice");
        g_free(msg);
    }
    g_socket_client_connect_to_host_async(cs.client, g->host, g->port,
                                          cs.cancel, connected,
                                          GUINT_TO_POINTER(cs.generation));
}

/* ---------------------------------------------------------- reading */

static void
read_done(GObject *src, GAsyncResult *res, gpointer gen)
{
    GError *err = NULL;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &err);
    char *nl;

    if (GPOINTER_TO_UINT(gen) != cs.generation) {
        if (err)
            g_error_free(err);
        return;
    }
    if (n < 0) {
        char *why = g_strdup_printf("Connection lost: %s", err->message);

        g_error_free(err);
        conn_close(why);
        g_free(why);
        return;
    }
    if (n == 0) {
        if (cs.inbuf->len)
            process_line(cs.inbuf->str);
        if (GPOINTER_TO_UINT(gen) == cs.generation)
            conn_close("The server closed the connection.");
        return;
    }
    g_string_append_len(cs.inbuf, cs.rbuf, n);
    while ((nl = memchr(cs.inbuf->str, '\n', cs.inbuf->len))) {
        gsize len = nl - cs.inbuf->str;
        char *line = g_strndup(cs.inbuf->str, len);

        g_string_erase(cs.inbuf, 0, len + 1);
        process_line(line);
        g_free(line);
        if (GPOINTER_TO_UINT(gen) != cs.generation)
            return;             /* the line closed the connection */
    }
    start_read();
}

static void
start_read(void)
{
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(cs.conn));

    g_input_stream_read_async(in, cs.rbuf, sizeof(cs.rbuf), G_PRIORITY_DEFAULT,
                              cs.cancel, read_done,
                              GUINT_TO_POINTER(cs.generation));
}

/* ------------------------------------------------------------ login */

static void
login_fail(const char *what, const char *text)
{
    g_free(cs.login_error);
    cs.login_error = text && *text
        ? g_strdup_printf("%s\n\nThe server said: %s", what, text)
        : g_strdup(what);
    send_line("quit");
    conn_close(NULL);
}

static void
login_next(void)
{
    switch (cs.ls) {
    case LS_INIT:
        cs.ls = LS_CLIENT;
        send_line("client " GTKACE_NAME " " GTKACE_VERSION);
        break;
    case LS_CLIENT:
        if (cs.game->utf8) {
            cs.ls = LS_OPTIONS;
            send_line("options utf-8");
            break;
        }
        /* fall through */
    case LS_OPTIONS:
        cs.ls = LS_USER;
        send_login("user", cs.game->user && *cs.game->user
                   ? cs.game->user : g_get_user_name());
        break;
    case LS_USER:
        cs.ls = LS_COUN;
        send_login("coun", cs.game->country);
        break;
    case LS_COUN:
        cs.ls = LS_PASS;
        send_login("pass", cs.game->password);
        break;
    case LS_PASS:
        if (cs.game->kill) {
            cs.ls = LS_KILL;
            send_line("kill");
            break;
        }
        /* fall through */
    case LS_KILL:
        cs.ls = LS_PLAY;
        send_line("play");
        break;
    default:
        break;
    }
}

static void
login_reply(int id, const char *text)
{
    if (id == C_DATA || id == C_FLASH || id == C_INFORM) {
        out_server_line(id, text, cs.utf8);
        return;
    }
    switch (cs.ls) {
    case LS_INIT:
        out_text(text, "server");
        if (id != C_INIT)
            login_fail("This does not look like an Empire server.", text);
        else
            login_next();
        break;
    case LS_CLIENT:             /* optional; old servers may refuse */
        login_next();
        break;
    case LS_OPTIONS:
        if (id == C_CMDOK)
            cs.utf8 = TRUE;
        else
            out_text("The server does not support UTF-8; using ASCII.",
                     "notice");
        login_next();
        break;
    case LS_USER:
        if (id != C_CMDOK)
            login_fail("The server refused the user name.", text);
        else
            login_next();
        break;
    case LS_COUN:
        if (id != C_CMDOK)
            login_fail("There is no country by that name.", text);
        else
            login_next();
        break;
    case LS_PASS:
        if (id != C_CMDOK)
            login_fail("The password is wrong.", text);
        else
            login_next();
        break;
    case LS_KILL:
        /* the reply is C_EXIT whether or not anything was killed */
        out_text(text, "server");
        login_next();
        break;
    case LS_PLAY:
        if (id == C_INIT) {
            int proto = atoi(text);

            if (proto != 2) {
                char *m = g_strdup_printf("The server uses protocol "
                                          "version %d; GtkACE knows "
                                          "version 2.", proto);
                login_fail(m, NULL);
                g_free(m);
                return;
            }
            cs.ls = LS_PLAYING;
            cs.prompted = FALSE;
            set_state(CONN_PLAYING, NULL);
            out_text("Successful logon.", "notice");
            mainwin_logged_in();
        } else {
            login_fail("The server would not let you play.", text);
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------- playing */

static void
send_next(void)
{
    EmpCmd *c;

    if (!cs.prompted || cs.current || cs.state != CONN_PLAYING)
        return;
    c = g_queue_pop_head(&cs.queue);
    if (!c) {
        status_set_busy(0);
        return;
    }
    cs.current = c;
    cs.prompted = FALSE;
    if (is_read_cmd(c->text))
        status_set_mail(NULL);
    if (c->flags & CMD_SHOW) {
        intel_command_start(c->text);
        tele_command_start(c->text);
    }
    if (c->flags & CMD_SHOW)
        out_prompt(c->text);
    send_line(c->text);
    status_set_busy(conn_queue_length());
}

static void
command_finished(void)
{
    EmpCmd *c = cs.current;

    cs.current = NULL;
    if (c) {
        if (c->flags & CMD_SHOW) {
            intel_command_end();
            tele_command_end();
        }
        if (c->done)
            c->done(c->data);
        if (c->flags & CMD_USER)
            refresh_after_command(c->text);
        cmd_free(c);
    }
}

/* "read" and "wire" in any abbreviation the server accepts */
static gboolean
is_read_cmd(const char *cmd)
{
    char word[16];

    if (sscanf(cmd, "%15s", word) != 1)
        return FALSE;
    return (strlen(word) >= 3 && !strncmp("read", word, strlen(word)))
        || (strlen(word) >= 3 && !strncmp("wire", word, strlen(word)));
}

/* Telegram notices come two ways: C_INFORM "[new tele]" when the
 * country has "toggle inform" on, otherwise a data line
 * "You have N new telegrams waiting ..." before the next prompt. */
static void
handle_inform(const char *text)
{
    gboolean tele = strstr(text, "new tele") != NULL;

    if (tele)
        status_set_mail("Telegrams waiting");
    else if (strstr(text, "new announcement"))
        status_set_mail("Announcements waiting");
    else
        return;
    if (cs.game->autoread && tele) {
        /* don't pile up reads */
        for (GList *l = cs.queue.head; l; l = l->next)
            if (is_read_cmd(((EmpCmd *)l->data)->text))
                return;
        if (cs.current && is_read_cmd(cs.current->text))
            return;
        tele_autoread();
    }
}

static void
play_line(int id, const char *text)
{
    EmpCmd *c = cs.current;

    switch (id) {
    case C_PROMPT: {
        int minutes = 0, btu = 0;

        sscanf(text, "%d %d", &minutes, &btu);
        status_set_btu(minutes, btu);
        g_clear_pointer(&cs.question, g_free);
        status_set_question(NULL);
        cs.prompted = TRUE;
        command_finished();
        send_next();
        if (!cs.current)
            status_set_busy(0);
        return;
    }
    case C_FLUSH:
        if (c && !g_queue_is_empty(&c->answers)) {
            /* prepared replies, such as the lines of a telegram */
            char *a = g_queue_pop_head(&c->answers);

            if (c->flags & CMD_SHOW)
                out_text(a, "answer");
            send_line(a);
            g_free(a);
            return;
        }
        if (c && c->responder) {
            char *plain = emp_text_decode(text, cs.utf8, NULL);
            char *a = c->responder(plain, c->data);

            g_free(plain);
            if (a) {
                if (c->flags & CMD_SHOW) {
                    out_text(text, "question");
                    out_text(a, "answer");
                }
                send_line(a);
                g_free(a);
                return;
            }
        }
        if (c && (c->flags & CMD_STOP) && text[0] == '<') {
            /* the end of a navigate or march: stop there */
            out_text(text, "question");
            out_text("h", "answer");
            send_line("h");
            return;
        }
        if (c && (c->flags & CMD_SHOW)) {
            g_free(cs.question);
            cs.question = g_strdup(text);
            status_set_question(text);
        } else {
            /* internal commands never expect questions: give up */
            out_text(text, "notice");
            send_line("aborted");
        }
        return;
    case C_INFORM:
        handle_inform(text);
        return;
    case C_FLASH:
        out_server_line(id, text, cs.utf8);
        return;
    case C_EXIT:
        out_text(text, "notice");
        return;
    case C_REDIR:
    case C_PIPE:
        out_text("GtkACE does not support redirecting output to files or "
                 "programs.", "notice");
        return;
    case C_EXECUTE:
        out_text("GtkACE does not support the execute command.", "notice");
        send_line("aborted");
        return;
    case C_NOECHO:
        return;
    default:
        break;
    }
    if (id == C_DATA && !strncmp(text, "You have ", 9)
        && strstr(text, " waiting"))
        handle_inform(text);
    if (c && c->line)
        c->line(id, text, c->data);
    if (c && (c->flags & CMD_SHOW) && id == C_DATA) {
        char *plain = emp_text_decode(text, cs.utf8, NULL);

        intel_line(plain);
        tele_line(plain);
        g_free(plain);
    }
    if (!c || (c->flags & CMD_SHOW))
        out_server_line(id, text, cs.utf8);
}

static void
process_line(const char *line)
{
    int id;
    const char *text;
    char ch = line[0];

    if (!ch)
        return;
    if (ch >= '0' && ch <= '9')
        id = ch - '0';
    else if (ch >= 'a' && ch <= 'z')
        id = ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'Z')
        id = ch - 'A' + 10;
    else
        id = C_DATA;
    /* text follows the id and one space; unknown ids count as data */
    text = line[1] == ' ' ? line + 2 : line + 1;
    if (id > C_INFORM)
        id = C_DATA;
    if (cs.ls == LS_PLAYING)
        play_line(id, text);
    else
        login_reply(id, text);
}

/* --------------------------------------------------------- commands */

static void
queue_command(const char *cmd, guint flags, CmdLineFunc line,
              const char *const *answers, CmdAnswerFunc responder,
              CmdDoneFunc done, gpointer data)
{
    EmpCmd *c;

    if (cs.state != CONN_PLAYING) {
        if (flags & CMD_USER)
            out_text("Not connected to a server.", "notice");
        if (done)
            done(data);
        return;
    }
    c = g_new0(EmpCmd, 1);
    c->text = g_strdup(cmd);
    c->flags = flags;
    c->line = line;
    c->responder = responder;
    c->done = done;
    c->data = data;
    for (int i = 0; answers && answers[i]; i++)
        g_queue_push_tail(&c->answers, g_strdup(answers[i]));
    g_queue_push_tail(&cs.queue, c);
    status_set_busy(conn_queue_length());
    send_next();
}

void
conn_command(const char *cmd, guint flags, CmdLineFunc line, CmdDoneFunc done,
             gpointer data)
{
    queue_command(cmd, flags, line, NULL, NULL, done, data);
}

void
conn_command_answers(const char *cmd, guint flags, const char *const *answers,
                     CmdDoneFunc done, gpointer data)
{
    queue_command(cmd, flags, NULL, answers, NULL, done, data);
}

void
conn_command_responder(const char *cmd, guint flags, CmdAnswerFunc responder,
                       CmdDoneFunc done, gpointer data)
{
    queue_command(cmd, flags, NULL, NULL, responder, done, data);
}

void
conn_answer(const char *text)
{
    if (!cs.question)
        return;
    out_text(text, "answer");
    g_clear_pointer(&cs.question, g_free);
    status_set_question(NULL);
    send_line(text);
}

void
conn_abort(void)
{
    if (cs.question) {
        out_text("aborted", "answer");
        g_clear_pointer(&cs.question, g_free);
        status_set_question(NULL);
        send_line("aborted");
    }
}
