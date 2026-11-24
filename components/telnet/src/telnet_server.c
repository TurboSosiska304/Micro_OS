/*
 * Micro OS — the shell over the air.
 *
 * One task, one port, one client at a time: the listener accepts a
 * connection, makes a session of the accounts on it (config/session.h), and
 * waits for that session to end. The session is a task of its own because a
 * task has one credential and no saved uid (ADR 0033): a session that became
 * a user cannot climb back, so the listener stays root and makes the next
 * session as root, which is what the init of the console does with its own.
 *
 * The bytes of the client are the terminal of the session: the parser of
 * telnet_proto.c strips the negotiation and gives one byte of the person at
 * a time, so `login`, `su` and the shell read the line where it was typed.
 * That is the defect of 1.6 this file fixes - the dialogue used to read the
 * UART of the machine.
 *
 * The echo is negotiated and not assumed: the server offers WILL ECHO, the
 * client that accepts stops echoing and the server repeats the bytes, and a
 * password is never repeated by either half (RFC 857). The output of the
 * session - the prompts, the answers of the commands, the log of what the
 * session does - goes to the socket because the socket is the terminal of
 * that task (kernel/console.h): the UART keeps its own session, and two
 * people of the machine do not read each other's answers.
 *
 * The sockets come from the kernel (kernel/net.h): the same server runs over
 * lwIP on the board and over the sockets of the host and of QEMU.
 */
#include "telnet/telnet.h"

#include <stdio.h>
#include <string.h>

#include "config/session.h"
#include "kernel/console.h"
#include "kernel/kmod.h"
#include "kernel/lock.h"
#include "kernel/net.h"
#include "kernel/panic.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kshell/kshell.h"
#include "telnet/telnet_proto.h"

#define TELNET_PORT 23

/* The offer of echo that opens a session: IAC WILL ECHO. */
static const char TELNET_OFFER[] = {(char)TELNET_IAC, (char)TELNET_WILL, (char)TELNET_OPT_ECHO};

/*
 * The wait for the next client. It is short because the loop of the server
 * looks at s_enabled between two waits: `telnet off` must close the door in
 * about a second whichever way it was typed. A session waits with the poll of
 * the port (see telnet_session).
 */
#define TELNET_ACCEPT_TIMEOUT_MS 1000

/*
 * The wait of the listener for one round of its session: a session that is
 * waiting for a key is not a session that ended, and `telnet off` is looked
 * at between two waits. Short enough that the door closes at once, long
 * enough that the listener is not a busy loop.
 */
#define TELNET_SESSION_POLL_MS 250

/* The room of the bytes of a person that came in one read of the socket,
 * with the negotiation taken out. One read is bounded by the buffer of the
 * session below, so the queue never spills. */
#define TELNET_CLIENT_READ 64u
#define TELNET_CLIENT_PENDING 64u

/*
 * The session of the client, and the state that goes with it, are one piece
 * of state: the task of the server writes it, the console and the command of
 * the shell read it. The mutex is recursive, so that a helper which takes
 * the lock may call another one that takes it too.
 *
 * The lock is held across the writes and the close of the session and not
 * across its reads: a write that does not wait and a close return at once,
 * while a read waits for a client and would hold the console behind it.
 * Holding it for the close is what keeps the session alive under the feet
 * of a console that is writing to it at that very moment.
 */
static k_mutex_t s_lock = K_MUTEX_INITIALIZER;
static k_socket_t *s_client;
static bool s_client_lost;

static bool s_started;
static volatile bool s_enabled = true;
static volatile bool s_listening;

/*
 * The terminal of one session: the socket, the state of the negotiation and
 * the bytes that came with the last read and were not used yet. The listener
 * fills it, the session task reads it, and it lives as long as the session
 * because the listener waits for the session to end.
 */
struct telnet_client {
    k_socket_t *socket;
    struct telnet_stream stream;
    char pending[TELNET_CLIENT_PENDING];
    k_size_t pending_len;
    k_size_t pending_at;
};

static void telnet_lock(void)
{
    if (k_mutex_lock(&s_lock) != 0) {
        /* Without the lock the client can be closed under the feet of the
         * shell, and the session has no way to know about it. */
        KERNEL_PANIC("telnet: cannot take the lock of the server");
    }
}

static void telnet_unlock(void)
{
    (void)k_mutex_unlock(&s_lock);
}

/*
 * The bytes of the session to the client. It waits for the acknowledgement:
 * the output of a session is the terminal of that session and a prompt that
 * was dropped is a person typing at a machine that asks nothing. The stack of
 * this port has one segment in flight, so a write that did not wait would be
 * dropped while the one before it is unacknowledged - the banner and the
 * first prompt of a session arrived together and the second one was lost.
 *
 * A client that stopped reading costs the session its patience and not the
 * console of the machine: the session waits for the timeout of the port and
 * then the client is lost, and every later write is a no-op.
 */
static void telnet_send(k_socket_t *socket, const char *data, k_size_t len)
{
    telnet_lock();

    if (socket != NULL && len > 0 && !s_client_lost) {
        if (k_socket_send(socket, data, len, true) < 0) {
            s_client_lost = true;
        }
    }

    telnet_unlock();
}

/*
 * The bytes the server writes to the terminal of the session. A terminal of a
 * wire needs the carriage back before the line feed: a client that gets LF
 * alone moves down and stays in the column it was.
 */
static void telnet_put(void *arg, const char *data, k_size_t len)
{
    struct telnet_client *client = arg;
    char out[TELNET_CLIENT_READ * 2];
    k_size_t used = 0;

    for (k_size_t i = 0; i < len; i++) {
        if (used + 2 >= sizeof(out)) {
            telnet_send(client->socket, out, used);
            used = 0;
        }

        if (data[i] == '\n' && (i == 0 || data[i - 1] != '\r')) {
            out[used++] = '\r';
        }

        out[used++] = data[i];
    }

    if (used > 0) {
        telnet_send(client->socket, out, used);
    }
}

/* Whether the server repeats the bytes of this client: the answer of the
 * negotiation, and false until the client accepts the offer. */
static bool telnet_echo(void *arg)
{
    struct telnet_client *client = arg;

    return client->stream.echo;
}

static bool telnet_client_lost(void);

/*
 * One byte of the person, waiting for it when there is none: the terminal
 * contract says that a quiet terminal is not a closed one. The negotiation
 * is answered as it comes, the bytes that came with the byte asked for are
 * kept in the queue, and -1 is the answer only when the session is over.
 */
static int telnet_get(void *arg)
{
    struct telnet_client *client = arg;
    char buf[TELNET_CLIENT_READ];

    for (;;) {
        char reply[TELNET_REPLY_MAX];
        size_t reply_len = 0;
        int n;
        k_size_t i;

        if (client->pending_at < client->pending_len) {
            return (int)(unsigned char)client->pending[client->pending_at++];
        }
        client->pending_len = 0;
        client->pending_at = 0;

        n = k_socket_recv(client->socket, buf, sizeof(buf));

        if (n < 0) {
            /*
             * The contract does not tell a client that is quiet from one
             * that is gone: a read that has nothing answers -1 after the
             * poll of the port. So the session ends on the answers it can
             * trust: the door being closed (telnet off) and a write that
             * failed.
             */
            if (!s_enabled || telnet_client_lost()) {
                return -1;
            }
            continue;
        }

        if (n == 0) {
            return -1; /* the client has left */
        }

        for (i = 0; i < (k_size_t)n; i++) {
            int data = telnet_stream_feed(&client->stream, (uint8_t)buf[i], reply, sizeof(reply),
                                          &reply_len);

            if (data >= 0 && client->pending_len < sizeof(client->pending)) {
                client->pending[client->pending_len++] = (char)data;
            }
        }

        if (reply_len > 0) {
            telnet_send(client->socket, reply, reply_len);
        }
    }
}

/* Let go of the client: after this call the console writes nowhere. */
static void telnet_forget_client(void)
{
    k_socket_t *client;

    telnet_lock();
    client = s_client;
    s_client = NULL;
    s_client_lost = false;
    /* Closing inside the lock: see the note at the declaration. */
    k_socket_close(client);
    telnet_unlock();
}

static bool telnet_client_lost(void)
{
    bool lost;

    telnet_lock();
    lost = s_client_lost;
    telnet_unlock();

    return lost;
}

/* One line of the state, for the doctor and for the command. */
static void telnet_state(char *buf, size_t size)
{
    bool in;

    if (!s_enabled) {
        snprintf(buf, size, "off");
        return;
    }

    if (!s_listening) {
        snprintf(buf, size, "port %d, not listening", TELNET_PORT);
        return;
    }

    telnet_lock();
    in = s_client != NULL;
    telnet_unlock();

    if (in) {
        snprintf(buf, size, "port %d, a client is in", TELNET_PORT);
    } else {
        snprintf(buf, size, "port %d, a login", TELNET_PORT);
    }
}

/*
 * The session with one client: a session task of the accounts on the socket,
 * and the listener waits for it. The task is what a `logout` can end; the
 * listener cannot lend its own credentials and take them back.
 */
static void telnet_session(k_socket_t *socket)
{
    static const char banner[] = "\r\nMicro OS telnet. The accounts of /etc/passwd.\r\n\r\n";
    struct telnet_client client;
    struct session_info info;
    k_u32_t pid;
    int status;

    memset(&client, 0, sizeof(client));
    client.socket = socket;
    telnet_stream_init(&client.stream);

    telnet_lock();
    s_client = socket;
    s_client_lost = false;
    telnet_unlock();

    telnet_send(socket, banner, sizeof(banner) - 1);
    telnet_send(socket, TELNET_OFFER, sizeof(TELNET_OFFER));

    memset(&info, 0, sizeof(info));
    info.tty = TASK_TTY_TELNET;
    info.terminal.ctx = &client;
    info.terminal.get = telnet_get;
    info.terminal.put = telnet_put;
    info.terminal.echo = telnet_echo;

    pid = session_start(&info);

    if (pid != 0) {
        /*
         * A session waits for a key in the get() of its terminal, and `telnet
         * off` has to reach it: the wait answers -1 when the door is closed,
         * and the loop here only watches for the end of the task and the
         * close of the socket. A client that is gone is noticed by a write
         * that fails (s_client_lost) or by the read of the peer (0).
         */
        while (task_wait(pid, &status, TELNET_SESSION_POLL_MS) != 0) {
            if (!s_enabled) {
                /* The session is in a read of at most the poll of the port;
                 * the loop keeps waiting for it to notice. */
                k_sched_sleep_ms(1);
            }
        }
    }

    telnet_forget_client();
}

/* The listener: a port of its own, waiting for the next client. */
static void telnet_task(void *arg)
{
    K_UNUSED(arg);

    for (;;) {
        k_server_t *server;

        if (!s_enabled) {
            k_sched_sleep_ms(500);
            continue;
        }

        server = k_server_listen(TELNET_PORT);
        if (server == NULL) {
            /* The port may still be in the hands of the stack for a while:
             * a couple of seconds and the door is tried again. */
            k_sched_sleep_ms(2000);
            continue;
        }

        s_listening = true;

        while (s_enabled) {
            k_socket_t *client = k_server_accept(server, TELNET_ACCEPT_TIMEOUT_MS);

            if (client != NULL) {
                telnet_session(client);
            }
        }

        s_listening = false;
        k_server_close(server);
    }
}

static int telnet_init(void)
{
    if (task_create("telnet", telnet_task, NULL, TASK_PRIO_DEFAULT) == NULL) {
        return -1;
    }

    s_started = true;

    return 0;
}

static int telnet_check(void)
{
    return s_started ? 0 : -1;
}

static const char *telnet_detail(void)
{
    static char buf[48];

    telnet_state(buf, sizeof(buf));

    return buf;
}

/* The command: the state, and the door on or off. */
static int cmd_telnet(int argc, char **argv)
{
    char state[48];

    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        s_enabled = false;
        kshell_printf("telnet: off\n");
        return KSHELL_OK;
    }

    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        s_enabled = true;
        kshell_printf("telnet: on\n");
        return KSHELL_OK;
    }

    if (argc == 1) {
        telnet_state(state, sizeof(state));
        kshell_printf("telnet: %s\n", state);
        return KSHELL_OK;
    }

    kshell_printf("usage: telnet [on|off]\n");
    return KSHELL_USAGE;
}

static const struct kmod s_telnet_module = {
    .name = "telnet",
    .desc = "the shell over the air on port 23, with the accounts of /etc",
    .init = telnet_init,
    .check = telnet_check,
    .detail = telnet_detail,
};

int telnet_register(void)
{
    int rc = kmod_register(&s_telnet_module);

    kshell_register("telnet", "the shell over the air: telnet [on|off]", cmd_telnet);

    return rc;
}
