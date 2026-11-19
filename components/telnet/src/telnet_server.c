/*
 * Micro OS — the shell over the air.
 *
 * One task, one port, one client at a time: the listener accepts a
 * connection, gives the bytes to the parser of telnet_proto.c and the lines
 * to the shell, and the output of the shell finds its way back through the
 * console — the client is registered as one more sink of it beside the UART
 * and the display.
 *
 * The answers of the negotiation are refusals (the parser of the protocol
 * does that): the client keeps its own echo and its own line editing, and
 * the session is plain text. A client that stopped reading loses its own
 * output, not the console of the board: every write to the socket is the
 * one that does not wait.
 *
 * The sockets come from the kernel (kernel/net.h): the same server runs over
 * lwIP on the board and over the sockets of the host under the tests.
 */
#include "telnet/telnet.h"

#include <stdio.h>
#include <string.h>

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

/*
 * The wait for the next client. It is short because the loop of the server
 * looks at s_enabled between two waits: `telnet off` must close the door in
 * about a second whichever way it was typed. A session waits with the poll
 * of the port (see telnet_session).
 */
#define TELNET_ACCEPT_TIMEOUT_MS 1000

static bool s_started;
static volatile bool s_enabled = true;
static volatile bool s_listening;

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

/* The session of the client, as the console sees it. */
static void telnet_console_write(const char *data, k_size_t len)
{
    telnet_lock();

    if (s_client != NULL) {
        /* wait = false: a client that stopped reading loses output, and
         * the console of the board does not wait for it. A failed write is
         * the only word of the contract about a peer that is gone. */
        if (k_socket_send(s_client, data, len, false) < 0) {
            s_client_lost = true;
        }
    }

    telnet_unlock();
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
        snprintf(buf, size, "port %d, no auth", TELNET_PORT);
    }
}

/* The session with one client: the bytes in, the lines to the shell. */
static void telnet_session(k_socket_t *client)
{
    static const char banner[] =
        "\r\nMicro OS telnet - no authentication, the local network only.\r\n\r\nmicroos> ";
    static const char prompt[] = "microos> ";
    struct telnet_parser parser;
    char reply[TELNET_REPLY_MAX];
    char line[TELNET_LINE_MAX];
    char buf[128];
    int n;
    int i;

    (void)k_socket_send(client, banner, sizeof(banner) - 1, false);

    telnet_lock();
    s_client = client;
    s_client_lost = false;
    telnet_unlock();

    telnet_parser_init(&parser);

    for (;;) {
        n = k_socket_recv(client, buf, sizeof(buf));

        if (n < 0) {
            /*
             * The contract does not tell a client that is quiet from one
             * that is gone: a read that has nothing answers -1 after the
             * poll of the port. So the session ends on the answers it can
             * trust — a client that closed (0), a write that failed, and
             * the board being told off.
             */
            if (!s_enabled || telnet_client_lost()) {
                break;
            }
            continue;
        }

        if (n == 0) {
            break; /* the client has left */
        }

        for (i = 0; i < n; i++) {
            size_t reply_len = 0;

            telnet_parser_feed(&parser, (uint8_t)buf[i], reply, sizeof(reply), &reply_len);

            if (reply_len > 0) {
                (void)k_socket_send(client, reply, reply_len, false);
            }

            if (telnet_parser_take_line(&parser, line, sizeof(line))) {
                if (line[0] != '\0') {
                    (void)kshell_execute(line);
                }

                /* The prompt comes back after the output of the command:
                 * the console sends it to this socket first. */
                (void)k_socket_send(client, prompt, sizeof(prompt) - 1, false);
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

    (void)k_console_add_sink(telnet_console_write);
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
    .desc = "the shell over the air on port 23",
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
