#define _POSIX_C_SOURCE 200809L

#include "filesystem.h"
#include "network.h"
#include "privilege.h"
#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_IP "127.0.0.1"
#define DEFAULT_PORT "8080"

enum {
    PENDING_CHILD = 1,
    PENDING_STOP = 2
};

static volatile sig_atomic_t pending_signals;
static int signal_pipe[2] = {-1, -1};
/* PIDs of the processes serving the connected clients. */
static pid_t *handlers;
static size_t handler_count;
static size_t handler_capacity;

static void handle_signal(int signal_number)
{
    int saved_errno = errno;
    ssize_t ignored;

    switch (signal_number) {
    case SIGCHLD:
        pending_signals |= PENDING_CHILD;
        break;
    case SIGINT:
    case SIGTERM:
        pending_signals |= PENDING_STOP;
        break;
    case SIGCONT:
        /* Wake poll so the console's foreground status is checked again. */
        break;
    default:
        break;
    }
    ignored = write(signal_pipe[1], "", 1);
    (void)ignored;
    errno = saved_errno;
}

static int create_signal_pipe(void)
{
    size_t i;

    if (pipe(signal_pipe) < 0) {
        return -1;
    }
    for (i = 0; i < 2; ++i) {
        int flags = fcntl(signal_pipe[i], F_GETFL);
        int descriptor_flags = fcntl(signal_pipe[i], F_GETFD);

        if (flags < 0 || descriptor_flags < 0 ||
            fcntl(signal_pipe[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(signal_pipe[i], F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
            int saved_errno = errno;
            close(signal_pipe[0]);
            close(signal_pipe[1]);
            signal_pipe[0] = -1;
            signal_pipe[1] = -1;
            errno = saved_errno;
            return -1;
        }
    }
    return 0;
}

static void track_handler(pid_t pid)
{
    if (handler_count == handler_capacity) {
        size_t capacity = handler_capacity == 0 ? 16 : handler_capacity * 2;
        pid_t *grown = realloc(handlers, capacity * sizeof(*handlers));
        if (grown == NULL) {
            fprintf(stderr, "warning: cannot track client handler %ld\n",
                    (long)pid);
            return;
        }
        handlers = grown;
        handler_capacity = capacity;
    }
    handlers[handler_count++] = pid;
}

static void reap_handlers(void)
{
    pid_t pid;

    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        size_t i;
        for (i = 0; i < handler_count; ++i) {
            if (handlers[i] == pid) {
                handlers[i] = handlers[--handler_count];
                break;
            }
        }
    }
}

static void stop_handlers(void)
{
    size_t i;

    /* Handlers run as other users after login: killing them needs root. */
    privilege_raise();
    for (i = 0; i < handler_count; ++i) {
        kill(handlers[i], SIGTERM);
    }
    privilege_lower();
    for (;;) {
        pid_t pid = waitpid(-1, NULL, 0);
        if (pid < 0 && errno == EINTR) {
            continue;
        }
        if (pid < 0) {
            break;
        }
    }
    free(handlers);
}

static int handle_pending_signals(const sigset_t *handled_signals)
{
    sigset_t previous_mask;
    sig_atomic_t signals;

    if (sigprocmask(SIG_BLOCK, handled_signals, &previous_mask) < 0) {
        return -1;
    }
    signals = pending_signals;
    pending_signals = 0;
    if (sigprocmask(SIG_SETMASK, &previous_mask, NULL) < 0) {
        return -1;
    }

    if ((signals & PENDING_CHILD) != 0) {
        reap_handlers();
    }
    if ((signals & PENDING_STOP) != 0) {
        return 1;
    }
    return 0;
}

static int drain_signal_pipe(void)
{
    char buffer[128];

    for (;;) {
        ssize_t count = read(signal_pipe[0], buffer, sizeof(buffer));

        if (count > 0) {
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        if (count == 0) {
            return 0;
        }
        return -1;
    }
}

static int console_in_foreground(void)
{
    return !isatty(STDIN_FILENO) || tcgetpgrp(STDIN_FILENO) == getpgrp();
}

/* Returns 1 on "exit", -1 when stdin is closed, 0 otherwise. */
static int handle_console(void)
{
    char buffer[256];
    char *save_pointer = NULL;
    char *line;
    ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer) - 1);

    if (count < 0) {
        return errno == EINTR || errno == EIO || errno == EAGAIN ? 0 : -1;
    }
    if (count == 0) {
        return -1;
    }
    buffer[count] = '\0';
    for (line = strtok_r(buffer, "\r\n", &save_pointer); line != NULL;
         line = strtok_r(NULL, "\r\n", &save_pointer)) {
        char word[16];
        char extra;
        int words = sscanf(line, "%15s %c", word, &extra);

        if (words == 1 && strcmp(word, "exit") == 0) {
            return 1;
        }
        if (words > 0) {
            printf("Unknown server command '%s'. Type 'exit' to stop the server.\n",
                   line);
        }
    }
    fflush(stdout);
    return 0;
}

static int install_signal_handlers(void)
{
    struct sigaction action = {0};

    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGCHLD);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGTERM);
    sigaddset(&action.sa_mask, SIGCONT);
    action.sa_handler = handle_signal;
    if (sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGCONT, &action, NULL) < 0) {
        return -1;
    }
    action.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &action, NULL) < 0) {
        return -1;
    }
    /* A background server must not be stopped when the shell owns the tty. */
    signal(SIGTTIN, SIG_IGN);
    return 0;
}

static void start_handler(int listener, int client_fd)
{
    pid_t child = fork();

    if (child < 0) {
        perror("fork");
    } else if (child == 0) {
        struct sigaction action = {0};

        close(listener);
        close(signal_pipe[0]);
        close(signal_pipe[1]);
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(SIGCHLD, &action, NULL);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGCONT, &action, NULL);
        session_serve(client_fd, ".");
        close(client_fd);
        _exit(EXIT_SUCCESS);
    } else {
        track_handler(child);
    }
    close(client_fd);
}

int main(int argc, char **argv)
{
    const char *ip = argc > 2 ? argv[2] : DEFAULT_IP;
    const char *port = argc > 3 ? argv[3] : DEFAULT_PORT;
    char root[PATH_MAX];
    char error[512];
    int listener;
    int watch_console = 1;
    sigset_t handled_signals;

    if (argc < 2 || argc > 4) {
        fprintf(stderr, "Usage: %s <root_directory> [IP] [port]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (fs_initialize_root(argv[1], error, sizeof(error)) < 0) {
        fprintf(stderr, "%s\n", error);
        return EXIT_FAILURE;
    }
    if (realpath(argv[1], root) == NULL) {
        perror("cannot resolve server root");
        return EXIT_FAILURE;
    }
    /* Handlers use paths relative to this cwd: after setuid the user may not
       be allowed to traverse the root's ancestors (e.g. a 0750 /home/x). */
    if (chdir(root) < 0) {
        perror("cannot enter server root");
        return EXIT_FAILURE;
    }
    if (create_signal_pipe() < 0) {
        perror("cannot create signal pipe");
        return EXIT_FAILURE;
    }
    if (install_signal_handlers() < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    listener = network_listen(ip, port, error, sizeof(error));
    if (listener < 0) {
        fprintf(stderr, "%s\n", error);
        return EXIT_FAILURE;
    }
    if (privilege_init() < 0) {
        perror("cannot drop root privileges");
        return EXIT_FAILURE;
    }
    printf("Server listening on %s:%s (root: %s). Type 'exit' to stop it.\n",
           ip, port, root);
    fflush(stdout);

    sigemptyset(&handled_signals);
    sigaddset(&handled_signals, SIGCHLD);
    sigaddset(&handled_signals, SIGINT);
    sigaddset(&handled_signals, SIGTERM);
    sigaddset(&handled_signals, SIGCONT);

    for (;;) {
        struct pollfd descriptors[3] = {
            { .fd = listener, .events = POLLIN },
            { .fd = signal_pipe[0], .events = POLLIN },
            { .fd = STDIN_FILENO, .events = POLLIN }
        };
        nfds_t descriptor_count = 2;
        int client_fd;
        int pending;

        pending = handle_pending_signals(&handled_signals);
        if (pending < 0) {
            perror("sigprocmask");
            break;
        }
        if (pending > 0) {
            break;
        }
        if (watch_console && console_in_foreground()) {
            descriptor_count = 3;
        }
        if (poll(descriptors, descriptor_count, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if (descriptors[1].revents != 0 && drain_signal_pipe() < 0) {
            perror("read signal pipe");
            break;
        }
        pending = handle_pending_signals(&handled_signals);
        if (pending < 0) {
            perror("sigprocmask");
            break;
        }
        if (pending > 0) {
            break;
        }
        if (descriptor_count == 3 && descriptors[2].revents != 0) {
            int console = handle_console();
            if (console > 0) {
                break;
            }
            if (console < 0) {
                watch_console = 0;
            }
        }
        if ((descriptors[0].revents & POLLIN) == 0) {
            continue;
        }
        client_fd = accept(listener, NULL, NULL);
        if (client_fd < 0) {
            if (errno != EINTR && errno != ECONNABORTED) {
                perror("accept");
            }
            continue;
        }
        start_handler(listener, client_fd);
    }

    printf("Server shutting down.\n");
    close(listener);
    stop_handlers();
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    return EXIT_SUCCESS;
}
