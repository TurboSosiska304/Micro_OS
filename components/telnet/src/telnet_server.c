/*
 * Micro OS — the shell over the air.
 *
 * One task, one port, one client at a time: the listener accepts a
 * connection, gives the bytes to the parser of telnet_proto.c and the
 * lines to the shell, and the output of the shell finds its way back
 * through the console — the client is registered as one more sink of
 * it beside the UART and the display.
 *
 * The answers of the negotiation are refusals (the parser of the
 * protocol does that): the client keeps its own echo and its own line
 * editing, and the session is plain text. A client that stopped
 * reading loses its own output, not the console of the board: the
 * writes to the socket do not wait.
 */
#include "telnet/telnet.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <lwip/sockets.h>

#include "kernel/console.h"
#include "kernel/kmod.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kshell/kshell.h"
#include "telnet/telnet_proto.h"

#define TELNET_PORT 23

static bool s_started;
static volatile bool s_enabled = true;
static volatile bool s_listening;
static volatile int s_client = -1;

/* The socket of the client, as the console sees it. */
static void telnet_console_write(const char *data, k_size_t len)
{
    int fd;

    k_port_enter_critical();
    fd = s_client;
    k_port_exit_critical();

    if (fd >= 0) {
        /* MSG_DONTWAIT: a client that stopped reading loses output. */
        (void)send(fd, data, len, MSG_DONTWAIT);
    }
}

/* One line of the state, for the doctor and for the command. */
static void telnet_state(char *buf, size_t size)
{
    int client;

    if (!s_enabled) {
        snprintf(buf, size, "off");
        return;
    }

    if (!s_listening) {
        snprintf(buf, size, "port %d, not listening", TELNET_PORT);
        return;
    }

    k_port_enter_critical();
    client = s_client;
    k_port_exit_critical();

    if (client >= 0) {
        snprintf(buf, size, "port %d, a client is in", TELNET_PORT);
    } else {
        snprintf(buf, size, "port %d, no auth", TELNET_PORT);
    }
}

/* The session with one client: the bytes in, the lines to the shell. */
static void telnet_session(int fd)
{
    static const char banner[] =
        "\r\nMicro OS telnet - no authentication, the local network only.\r\n\r\nmicroos> ";
    static const char prompt[] = "microos> ";
    struct telnet_parser parser;
    struct timeval timeout;
    char reply[TELNET_REPLY_MAX];
    char line[TELNET_LINE_MAX];
    char buf[128];
    int n;
    int i;

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    (void)send(fd, banner, sizeof(banner) - 1, MSG_DONTWAIT);

    k_port_enter_critical();
    s_client = fd;
    k_port_exit_critical();

    telnet_parser_init(&parser);

    for (;;) {
        n = (int)recv(fd, buf, sizeof(buf), 0);

        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                /* One second of silence: has the board been told off? */
                if (!s_enabled) {
                    break;
                }
                continue;
            }
            break;
        }

        if (n == 0) {
            break; /* the client has left */
        }

        for (i = 0; i < n; i++) {
            size_t reply_len = 0;

            telnet_parser_feed(&parser, (uint8_t)buf[i], reply, sizeof(reply), &reply_len);

            if (reply_len > 0) {
                (void)send(fd, reply, reply_len, MSG_DONTWAIT);
            }

            if (telnet_parser_take_line(&parser, line, sizeof(line))) {
                if (line[0] != '\0') {
                    (void)kshell_execute(line);
                }

                /* The prompt comes back after the output of the
                 * command: the console sends it to this socket first. */
                (void)send(fd, prompt, sizeof(prompt) - 1, MSG_DONTWAIT);
            }
        }
    }

    k_port_enter_critical();
    s_client = -1;
    k_port_exit_critical();
}

/* The listener: a port of its own, waiting for the next client. */
static void telnet_task(void *arg)
{
    K_UNUSED(arg);

    for (;;) {
        struct sockaddr_in address;
        int server;
        int one = 1;

        if (!s_enabled) {
            k_sched_sleep_ms(500);
            continue;
        }

        server = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (server < 0) {
            k_sched_sleep_ms(2000);
            continue;
        }

        (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(TELNET_PORT);
        address.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
            listen(server, 1) != 0) {
            close(server);
            k_sched_sleep_ms(2000);
            continue;
        }

        s_listening = true;

        for (;;) {
            fd_set read_fds;
            struct timeval one_second;
            int ready;

            if (!s_enabled) {
                break;
            }

            FD_ZERO(&read_fds);
            FD_SET(server, &read_fds);

            one_second.tv_sec = 1;
            one_second.tv_usec = 0;

            ready = select(server + 1, &read_fds, NULL, NULL, &one_second);
            if (ready > 0 && FD_ISSET(server, &read_fds)) {
                int client = accept(server, NULL, NULL);

                if (client >= 0) {
                    telnet_session(client);
                    close(client);
                }
            }
        }

        s_listening = false;
        close(server);
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
