#define _POSIX_C_SOURCE 200809L

#include "client_transfer.h"
#include "network.h"
#include "utils.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define DEFAULT_IP "127.0.0.1"
#define DEFAULT_PORT "8080"

static int append_write_input(unsigned char **contents, size_t *length,
                              size_t *capacity, const unsigned char *data,
                              size_t data_length)
{
    size_t needed;

    if (data_length > NETWORK_MAX_FRAME - *length) {
        errno = EMSGSIZE;
        return -1;
    }
    needed = *length + data_length;
    if (needed > *capacity) {
        size_t new_capacity = *capacity == 0 ? 1024 : *capacity;
        unsigned char *new_contents;

        while (new_capacity < needed) {
            if (new_capacity > NETWORK_MAX_FRAME / 2) {
                new_capacity = NETWORK_MAX_FRAME;
                break;
            }
            new_capacity *= 2;
        }
        new_contents = realloc(*contents, new_capacity);
        if (new_contents == NULL) {
            return -1;
        }
        *contents = new_contents;
        *capacity = new_capacity;
    }
    memcpy(*contents + *length, data, data_length);
    *length = needed;
    return 0;
}

static int collect_terminal_input(unsigned char **contents, size_t *length,
                                  size_t *capacity)
{
    struct termios original;
    struct termios input_mode;
    unsigned char character;
    int result = -1;
    int saved_errno = 0;

    if (tcgetattr(STDIN_FILENO, &original) < 0) {
        print_error("cannot read terminal settings");
        return -1;
    }
    input_mode = original;
    input_mode.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    input_mode.c_cc[VMIN] = 1;
    input_mode.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &input_mode) < 0) {
        print_error("cannot configure terminal");
        return -1;
    }

    for (;;) {
        ssize_t count = read(STDIN_FILENO, &character, sizeof(character));

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            saved_errno = errno;
            print_error("cannot read terminal input");
            break;
        }
        if (count == 0 || character == '\004') {
            result = 0;
            break;
        }
        if (character == '\003') {
            printf("\nERROR: write cancelled; disconnecting\n");
            break;
        }
        if (character == '\b' || character == '\177') {
            if (*length > 0) {
                --*length;
                while (*length > 0 &&
                       ((*contents)[*length] & 0xc0) == 0x80) {
                    --*length;
                }
                if (write_all(STDOUT_FILENO, "\b \b", 3) < 0) {
                    saved_errno = errno;
                    print_error("cannot update terminal");
                    break;
                }
            }
            continue;
        }
        if (character == '\r' || character == '\n') {
            character = '\n';
        }
        if (append_write_input(contents, length, capacity, &character, 1) < 0) {
            saved_errno = errno;
            print_error(saved_errno == EMSGSIZE
                            ? "input exceeds the maximum frame size"
                            : "cannot collect file content");
            break;
        }
        if (write_all(STDOUT_FILENO, &character, 1) < 0) {
            saved_errno = errno;
            print_error("cannot update terminal");
            break;
        }
    }

    if (tcsetattr(STDIN_FILENO, TCSANOW, &original) < 0) {
        print_error("cannot restore terminal settings");
        result = -1;
    }
    if (result == 0 && write_all(STDOUT_FILENO, "\n", 1) < 0) {
        print_error("cannot update terminal");
        result = -1;
    }
    if (saved_errno != 0) {
        errno = saved_errno;
    }
    return result;
}

/* Collects content until Ctrl+D in a terminal or EOF in redirected input. */
static int collect_write_input(unsigned char **contents, size_t *length)
{
    char *line = NULL;
    size_t line_capacity = 0;
    size_t capacity = 0;
    ssize_t line_length;
    int result = -1;

    *contents = NULL;
    *length = 0;
    printf("Enter file content; press Ctrl+D to finish.\n");
    if (isatty(STDIN_FILENO)) {
        result = collect_terminal_input(contents, length, &capacity);
    } else {
        while ((line_length = getline(&line, &line_capacity, stdin)) >= 0) {
            if (append_write_input(contents, length, &capacity,
                                   (unsigned char *)line,
                                   (size_t)line_length) < 0) {
                print_error(errno == EMSGSIZE
                                ? "input exceeds the maximum frame size"
                                : "cannot collect file content");
                goto done;
            }
        }
        if (feof(stdin)) {
            result = 0;
        } else {
            print_error("cannot read file content");
        }
    }

done:
    free(line);
    if (result < 0) {
        free(*contents);
        *contents = NULL;
        *length = 0;
    }
    return result;
}

/* Recognizes "<name> <path>" and "<name> -offset=<num> <path>". */
static int is_offset_command(const char *command, const char *name)
{
    char *copy = strdup(command);
    char *arguments[4];
    size_t count;
    off_t offset;
    int valid;

    if (copy == NULL) {
        return 0;
    }
    count = split_arguments(copy, arguments, 4);
    valid = (count == 2 || count == 3) && strcmp(arguments[0], name) == 0 &&
            (count == 2 || parse_offset_option(arguments[1], &offset) == 0);
    free(copy);
    return valid;
}

/* Prints an asynchronous message (a transfer notification). */
static int print_notification(int socket_fd)
{
    const size_t prefix_length = strlen(NETWORK_NOTIFY_PREFIX);
    unsigned char *message;
    size_t length;
    int valid;

    if (receive_frame(socket_fd, &message, &length) < 0) {
        print_error("server disconnected");
        return -1;
    }
    valid = length >= prefix_length &&
            memcmp(message, NETWORK_NOTIFY_PREFIX, prefix_length) == 0;
    if (valid) {
        fwrite(message + prefix_length, 1, length - prefix_length, stdout);
    } else {
        printf("ERROR: unexpected asynchronous server message\n");
    }
    free(message);
    return valid ? 0 : -1;
}

/* upload/download run on their own connection, from the current directory. */
static int run_transfer(int socket_fd, const char *ip, const char *port,
                        const char *username, transfer_command *transfer,
                        background_jobs *jobs)
{
    if (username[0] == '\0') {
        printf("ERROR: log in before starting a file transfer\n");
        return 0;
    }
    if (query_cwd(socket_fd, transfer->cwd, sizeof(transfer->cwd)) < 0) {
        print_error("cannot read the current directory");
        return -1;
    }
    if (transfer->background) {
        start_background_transfer(jobs, ip, port, username, socket_fd, transfer);
    } else if (perform_transfer(ip, port, username, transfer) == 0 &&
               strcmp(transfer->operation, "download") == 0) {
        printf("OK: download complete\n");
    }
    return 0;
}

/* Sends any other command and prints the answer; -1 if the connection broke. */
static int run_command(int socket_fd, const char *line,
                       char *logged_in_user, size_t user_size)
{
    char login_candidate[128] = "";
    unsigned char *response;
    size_t response_length;
    int writing = is_offset_command(line, "write");

    if (is_offset_command(line, "read")) {
        return read_remote_file(socket_fd, line) < 0 ? -1 : 0;
    }
    /* The server would not read the content, desynchronizing the protocol. */
    if (writing && logged_in_user[0] == '\0') {
        printf("ERROR: log in first with: login <username>\n");
        return 0;
    }
    if (sscanf(line, "login %127s", login_candidate) != 1) {
        login_candidate[0] = '\0';
    }
    if (send_text(socket_fd, line) < 0) {
        print_error("cannot send command");
        return -1;
    }
    if (writing) {
        unsigned char *contents;
        size_t contents_length;
        int sent;

        if (collect_write_input(&contents, &contents_length) < 0) {
            printf("ERROR: could not collect file content; disconnecting\n");
            return -1;
        }
        sent = send_frame(socket_fd, contents, contents_length);
        free(contents);
        if (sent < 0) {
            print_error("cannot send file content");
            return -1;
        }
    }
    if (receive_response(socket_fd, &response, &response_length) < 0) {
        print_error("server disconnected");
        return -1;
    }
    fwrite(response, 1, response_length, stdout);
    if (login_candidate[0] != '\0' && response_length >= 2 &&
        memcmp(response, "OK", 2) == 0) {
        snprintf(logged_in_user, user_size, "%s", login_candidate);
    }
    free(response);
    return 0;
}

int main(int argc, char **argv)
{
    const char *ip = argc > 1 ? argv[1] : DEFAULT_IP;
    const char *port = argc > 2 ? argv[2] : DEFAULT_PORT;
    char *line = NULL;
    size_t line_capacity = 0;
    char logged_in_user[128] = "";
    background_jobs jobs = {0};
    char error[512];
    int socket_fd;

    if (argc > 3) {
        printf("Usage: %s [IP] [port]\n", argv[0]);
        return EXIT_FAILURE;
    }
    /* poll() watches the fd: stdio must not hide already-read lines from it. */
    setvbuf(stdin, NULL, _IONBF, 0);
    socket_fd = network_connect(ip, port, error, sizeof(error));
    if (socket_fd < 0) {
        printf("ERROR: %s\n", error);
        return EXIT_FAILURE;
    }
    printf("Connected to %s:%s. Type 'exit' to quit.\n", ip, port);

    for (;;) {
        struct pollfd descriptors[2] = {
            { .fd = STDIN_FILENO, .events = POLLIN },
            { .fd = socket_fd, .events = POLLIN }
        };
        transfer_command transfer = {0};
        ssize_t line_length;
        int kind;

        reap_background_jobs(&jobs);
        printf("> ");
        fflush(stdout);
        /* Waits for user input or for a notification from the server. */
        if (poll(descriptors, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            print_error("poll failed");
            break;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            if (print_notification(socket_fd) < 0) {
                break;
            }
            continue;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            printf("ERROR: server connection closed\n");
            break;
        }
        if ((descriptors[0].revents & (POLLIN | POLLHUP)) == 0) {
            continue;
        }
        line_length = getline(&line, &line_capacity, stdin);
        if (line_length < 0) {
            wait_background_jobs(&jobs);
            break;
        }
        while (line_length > 0 &&
               (line[line_length - 1] == '\n' || line[line_length - 1] == '\r')) {
            line[--line_length] = '\0';
        }
        if (line_length == 0) {
            continue;
        }
        if ((size_t)line_length > NETWORK_MAX_FRAME) {
            printf("ERROR: command is too long\n");
            continue;
        }
        if (strcmp(line, "exit") == 0) {
            /* A job may have ended while we were waiting for input. */
            reap_background_jobs(&jobs);
            if (jobs.count > 0) {
                printf("%zu background operation(s) still running; "
                       "termination cancelled.\n", jobs.count);
                continue;
            }
            break;
        }
        kind = parse_transfer_command(line, &transfer);
        if (kind < 0) {
            printf("ERROR: usage: upload [-b] <client path> <server path> or "
                   "download [-b] <server path> <client path>\n");
        } else if (kind > 0) {
            if (run_transfer(socket_fd, ip, port, logged_in_user, &transfer,
                             &jobs) < 0) {
                break;
            }
        } else if (run_command(socket_fd, line, logged_in_user,
                               sizeof(logged_in_user)) < 0) {
            break;
        }
    }

    free(line);
    close(socket_fd);
    return EXIT_SUCCESS;
}
