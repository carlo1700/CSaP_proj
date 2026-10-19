#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "filesystem.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netdb.h>
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
#define COMMAND_LIMIT 4096
#define TRANSFER_CHUNK (64U * 1024U)

static volatile sig_atomic_t stop_server;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_server = 1;
}

static void reap_children(int signal_number)
{
    (void)signal_number;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

static int send_text(int client_fd, const char *text)
{
    return send_frame(client_fd, text, strlen(text));
}

static int parse_permissions(const char *text, mode_t *permissions)
{
    char *end;
    unsigned long value;
    size_t i;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    for (i = 0; text[i] != '\0'; ++i) {
        if (text[i] < '0' || text[i] > '7') {
            return -1;
        }
    }
    errno = 0;
    value = strtoul(text, &end, 8);
    if (errno != 0 || *end != '\0' || value > 0777) {
        return -1;
    }
    *permissions = (mode_t)value;
    return 0;
}

static int parse_offset(const char *option, off_t *offset)
{
    char *end;
    long long value;

    if (strncmp(option, "-offset=", 8) != 0 || option[8] == '\0') {
        return -1;
    }
    errno = 0;
    value = strtoll(option + 8, &end, 10);
    if (errno != 0 || *end != '\0' || value < 0) {
        return -1;
    }
    *offset = (off_t)value;
    return 0;
}

static int send_error(int client_fd, const char *message)
{
    char response[1024];
    snprintf(response, sizeof(response), "ERROR: %s\n", message);
    return send_text(client_fd, response);
}

static int write_all(int file_fd, const unsigned char *data, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t result = write(file_fd, data + written, length - written);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

static int receive_upload(int client_fd, const fs_session *session,
                          const char *path)
{
    char error[512];
    int file_fd;

    if (fs_upload_begin(session, path, &file_fd, error, sizeof(error)) < 0) {
        for (;;) {
            unsigned char *discarded;
            size_t discarded_length;
            if (receive_frame(client_fd, &discarded, &discarded_length) < 0) {
                return -1;
            }
            free(discarded);
            if (discarded_length == 0) {
                break;
            }
        }
        return send_error(client_fd, error);
    }

    for (;;) {
        unsigned char *chunk;
        size_t chunk_length;

        if (receive_frame(client_fd, &chunk, &chunk_length) < 0) {
            fs_upload_finish(file_fd, 0, error, sizeof(error));
            return -1;
        }
        if (chunk_length == 0) {
            free(chunk);
            break;
        }
        if (write_all(file_fd, chunk, chunk_length) < 0) {
            free(chunk);
            fs_upload_finish(file_fd, 0, error, sizeof(error));
            return -1;
        }
        free(chunk);
    }

    if (fs_upload_finish(file_fd, 1, error, sizeof(error)) < 0) {
        return send_error(client_fd, error);
    }
    return send_text(client_fd, "OK: upload complete\n");
}

static int send_download(int client_fd, const fs_session *session,
                         const char *path)
{
    unsigned char buffer[TRANSFER_CHUNK];
    char error[512];
    int file_fd;

    if (fs_download_open(session, path, &file_fd, error, sizeof(error)) < 0) {
        return send_error(client_fd, error);
    }
    if (send_text(client_fd, "OK: download begins\n") < 0) {
        close(file_fd);
        return -1;
    }
    for (;;) {
        ssize_t count = read(file_fd, buffer, sizeof(buffer));

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            close(file_fd);
            return -1;
        }
        if (count == 0) {
            break;
        }
        if (send_frame(client_fd, buffer, (size_t)count) < 0) {
            close(file_fd);
            return -1;
        }
    }
    close(file_fd);
    return send_frame(client_fd, NULL, 0);
}

static int send_read(int client_fd, const fs_session *session,
                     const char *path, off_t offset)
{
    unsigned char buffer[TRANSFER_CHUNK];
    char error[512];
    int file_fd;

    if (fs_download_open(session, path, &file_fd, error, sizeof(error)) < 0) {
        return send_error(client_fd, error);
    }
    if (lseek(file_fd, offset, SEEK_SET) < 0) {
        snprintf(error, sizeof(error), "cannot seek file: %s", strerror(errno));
        close(file_fd);
        return send_error(client_fd, error);
    }
    if (send_text(client_fd, "OK: read begins\n") < 0) {
        close(file_fd);
        return -1;
    }
    for (;;) {
        ssize_t count = read(file_fd, buffer, sizeof(buffer));

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            close(file_fd);
            return -1;
        }
        if (count == 0) {
            break;
        }
        if (send_frame(client_fd, buffer, (size_t)count) < 0) {
            close(file_fd);
            return -1;
        }
    }
    close(file_fd);
    return send_frame(client_fd, NULL, 0);
}

static int execute_command(int client_fd, const char *root, fs_session *session,
                           int *logged_in, char *command)
{
    char *arguments[6] = {0};
    size_t argument_count = 0;
    char *save_pointer = NULL;
    char *token;
    char error[512];
    mode_t permissions;

    for (token = strtok_r(command, " \t\r\n", &save_pointer);
         token != NULL && argument_count < 6;
         token = strtok_r(NULL, " \t\r\n", &save_pointer)) {
        arguments[argument_count++] = token;
    }
    if (argument_count == 0) {
        return send_error(client_fd, "empty command");
    }
    if (argument_count == 6) {
        return send_error(client_fd, "too many command arguments");
    }

    if (strcmp(arguments[0], "login") == 0) {
        if (argument_count != 2) {
            return send_error(client_fd, "usage: login <username>");
        }
        if (fs_login(session, root, arguments[1], error, sizeof(error)) < 0) {
            return send_error(client_fd, error);
        }
        if (geteuid() == 0) {
            if (initgroups(session->username, session->gid) < 0 ||
                setgid(session->gid) < 0 || setuid(session->uid) < 0) {
                return send_error(client_fd, "cannot drop privileges to user account");
            }
        } else if (session->uid != geteuid()) {
            return send_error(client_fd,
                              "server must run as root to switch to this user account");
        }
        *logged_in = 1;
        return send_text(client_fd, "OK: logged in\n");
    }
    if (strcmp(arguments[0], "create_user") == 0) {
        if (argument_count != 3 ||
            parse_permissions(arguments[2], &permissions) < 0) {
            return send_error(client_fd,
                              "usage: create_user <username> <permissions in octal>");
        }
        if (fs_create_user(root, arguments[1], permissions,
                           error, sizeof(error)) < 0) {
            return send_error(client_fd, error);
        }
        return send_text(client_fd, "OK: user created\n");
    }
    if (strcmp(arguments[0], "exit") == 0) {
        return 1;
    }
    if (!*logged_in) {
        return send_error(client_fd, "log in first with: login <username>");
    }

    if (strcmp(arguments[0], "create") == 0) {
        int directory = 0;
        const char *path;
        const char *mode;

        if (argument_count == 4 && strcmp(arguments[1], "-d") == 0) {
            directory = 1;
            path = arguments[2];
            mode = arguments[3];
        } else if (argument_count == 4 && strcmp(arguments[3], "-d") == 0) {
            directory = 1;
            path = arguments[1];
            mode = arguments[2];
        } else if (argument_count == 3) {
            path = arguments[1];
            mode = arguments[2];
        } else {
            return send_error(client_fd, "usage: create [-d] <path> <permissions>");
        }
        if (parse_permissions(mode, &permissions) < 0) {
            return send_error(client_fd, "permissions must be octal (0000-0777)");
        }
        if (fs_create(session, path, permissions, directory,
                      error, sizeof(error)) < 0) {
            return send_error(client_fd, error);
        }
        return send_text(client_fd, "OK: created\n");
    }

    if (strcmp(arguments[0], "chmod") == 0) {
        if (argument_count != 3 ||
            parse_permissions(arguments[2], &permissions) < 0) {
            return send_error(client_fd, "usage: chmod <path> <permissions>");
        }
        if (fs_chmod(session, arguments[1], permissions,
                     error, sizeof(error)) < 0) {
            return send_error(client_fd, error);
        }
        return send_text(client_fd, "OK: permissions changed\n");
    }

    if (strcmp(arguments[0], "move") == 0) {
        if (argument_count != 3) {
            return send_error(client_fd, "usage: move <path1> <path2>");
        }
        if (fs_move(session, arguments[1], arguments[2],
                    error, sizeof(error)) < 0) {
            return send_error(client_fd, error);
        }
        return send_text(client_fd, "OK: moved\n");
    }

    if (strcmp(arguments[0], "cd") == 0) {
        if (argument_count != 2) {
            return send_error(client_fd, "usage: cd <path>");
        }
        if (fs_change_directory(session, arguments[1],
                                error, sizeof(error)) < 0) {
            return send_error(client_fd, error);
        }
        return send_text(client_fd, "OK: directory changed\n");
    }

    if (strcmp(arguments[0], "list") == 0) {
        char *listing;
        size_t listing_length;
        int result;

        if (argument_count > 2) {
            return send_error(client_fd, "usage: list [path | @username[/path]]");
        }
        if (argument_count == 2 && arguments[1][0] == '@') {
            char *other_path = strchr(arguments[1] + 1, '/');
            if (other_path != NULL) {
                *other_path++ = '\0';
            }
            result = fs_list_other(session, arguments[1] + 1,
                                   other_path == NULL ? "." : other_path,
                                   &listing, &listing_length,
                                   error, sizeof(error));
        } else {
            result = fs_list(session, argument_count == 2 ? arguments[1] : ".",
                             &listing, &listing_length, error, sizeof(error));
        }
        if (result < 0) {
            return send_error(client_fd, error);
        }
        if (send_frame(client_fd, listing, listing_length) < 0) {
            free(listing);
            return -1;
        }
        free(listing);
        return 0;
    }

    if (strcmp(arguments[0], "upload") == 0) {
        if (argument_count != 2) {
            return send_error(client_fd, "usage: upload <server path>");
        }
        return receive_upload(client_fd, session, arguments[1]);
    }

    if (strcmp(arguments[0], "download") == 0) {
        if (argument_count != 2) {
            return send_error(client_fd, "usage: download <server path>");
        }
        return send_download(client_fd, session, arguments[1]);
    }

    if (strcmp(arguments[0], "read") == 0) {
        off_t offset = 0;
        const char *path;

        if (argument_count == 3 &&
            parse_offset(arguments[1], &offset) == 0) {
            path = arguments[2];
        } else if (argument_count == 2) {
            path = arguments[1];
        } else {
            return send_error(client_fd,
                              "usage: read [-offset=<num>] <path>");
        }
        return send_read(client_fd, session, path, offset);
    }

    if (strcmp(arguments[0], "write") == 0) {
        off_t offset = 0;
        const char *path;
        unsigned char *contents;
        size_t contents_length;

        if (argument_count == 3 &&
            parse_offset(arguments[1], &offset) == 0) {
            path = arguments[2];
        } else if (argument_count == 2) {
            path = arguments[1];
        } else {
            return send_error(client_fd,
                              "usage: write [-offset=<num>] <path>");
        }
        if (receive_frame(client_fd, &contents, &contents_length) < 0) {
            return -1;
        }
        if (fs_write(session, path, offset, contents, contents_length,
                     error, sizeof(error)) < 0) {
            free(contents);
            return send_error(client_fd, error);
        }
        free(contents);
        return send_text(client_fd, "OK: file written\n");
    }

    if (strcmp(arguments[0], "delete") == 0) {
        if (argument_count != 2) {
            return send_error(client_fd, "usage: delete <path>");
        }
        if (fs_delete(session, arguments[1], error, sizeof(error)) < 0) {
            return send_error(client_fd, error);
        }
        return send_text(client_fd, "OK: deleted\n");
    }

    return send_error(client_fd, "unknown command");
}

static void serve_client(int client_fd, const char *root)
{
    fs_session session = {0};
    int logged_in = 0;

    for (;;) {
        unsigned char *request;
        size_t request_length;
        int result;

        if (receive_frame(client_fd, &request, &request_length) < 0) {
            break;
        }
        if (request_length > COMMAND_LIMIT ||
            memchr(request, '\0', request_length) != NULL) {
            free(request);
            if (send_error(client_fd, "invalid or oversized command") < 0) {
                break;
            }
            continue;
        }

        result = execute_command(client_fd, root, &session, &logged_in,
                                 (char *)request);
        free(request);
        if (result != 0) {
            break;
        }
    }
}

static int create_listener(const char *ip, const char *port)
{
    struct addrinfo hints = {0};
    struct addrinfo *addresses;
    struct addrinfo *address;
    int listener = -1;
    int option = 1;
    int result;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    result = getaddrinfo(ip, port, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "Invalid server address: %s\n", gai_strerror(result));
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        listener = socket(address->ai_family, address->ai_socktype,
                          address->ai_protocol);
        if (listener < 0) {
            continue;
        }
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
        if (bind(listener, address->ai_addr, address->ai_addrlen) == 0 &&
            listen(listener, 32) == 0) {
            break;
        }
        close(listener);
        listener = -1;
    }
    freeaddrinfo(addresses);
    if (listener < 0) {
        perror("cannot bind server socket");
    }
    return listener;
}

int main(int argc, char **argv)
{
    const char *ip = argc > 2 ? argv[2] : DEFAULT_IP;
    const char *port = argc > 3 ? argv[3] : DEFAULT_PORT;
    char root[PATH_MAX];
    char error[512];
    struct sigaction action = {0};
    int listener;

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

    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGINT, &action, NULL) < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    action.sa_handler = reap_children;
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &action, NULL) < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    listener = create_listener(ip, port);
    if (listener < 0) {
        return EXIT_FAILURE;
    }
    printf("Server listening on %s:%s (root: %s)\n", ip, port, root);
    fflush(stdout);

    while (!stop_server) {
        int client_fd = accept(listener, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        pid_t child = fork();
        if (child < 0) {
            perror("fork");
            close(client_fd);
            continue;
        }
        if (child == 0) {
            close(listener);
            action.sa_handler = SIG_DFL;
            action.sa_flags = 0;
            sigaction(SIGCHLD, &action, NULL);
            serve_client(client_fd, root);
            close(client_fd);
            _exit(EXIT_SUCCESS);
        }
        close(client_fd);
    }

    close(listener);
    return EXIT_SUCCESS;
}
