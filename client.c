#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_IP "127.0.0.1"
#define DEFAULT_PORT "8080"
#define TRANSFER_CHUNK (64U * 1024U)
#define MAX_BACKGROUND_JOBS 64

typedef struct {
    char operation[16];
    char first_path[PATH_MAX];
    char second_path[PATH_MAX];
    int background;
} transfer_command;

static int receive_server_frame(int socket_fd, unsigned char **data,
                                size_t *length)
{
    static const unsigned char notification_prefix[] = "\001NOTIFY ";

    for (;;) {
        if (receive_frame(socket_fd, data, length) < 0) {
            return -1;
        }
        if (*length >= sizeof(notification_prefix) - 1 &&
            memcmp(*data, notification_prefix,
                   sizeof(notification_prefix) - 1) == 0) {
            fwrite(*data + sizeof(notification_prefix) - 1, 1,
                   *length - (sizeof(notification_prefix) - 1), stdout);
            fflush(stdout);
            free(*data);
            *data = NULL;
            continue;
        }
        return 0;
    }
}

static int connect_to_server(const char *ip, const char *port)
{
    struct addrinfo hints = {0};
    struct addrinfo *addresses;
    struct addrinfo *address;
    int socket_fd = -1;
    int result;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    result = getaddrinfo(ip, port, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "Invalid server address: %s\n", gai_strerror(result));
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype,
                           address->ai_protocol);
        if (socket_fd >= 0 &&
            connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        if (socket_fd >= 0) {
            close(socket_fd);
            socket_fd = -1;
        }
    }
    freeaddrinfo(addresses);
    if (socket_fd < 0) {
        perror("cannot connect to server");
    }
    return socket_fd;
}

static int read_file_chunk(int file_fd, unsigned char *buffer, size_t capacity,
                           size_t *length)
{
    for (;;) {
        ssize_t result = read(file_fd, buffer, capacity);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0) {
            return -1;
        }
        *length = (size_t)result;
        return 0;
    }
}

static int download_file(int socket_fd, const char *server_path,
                         const char *client_path)
{
    char command[PATH_MAX + 32];
    unsigned char *frame;
    size_t frame_length;
    size_t command_length;
    int file_fd = -1;
    int result = -1;

    if (snprintf(command, sizeof(command), "download %s", server_path) >=
        (int)sizeof(command)) {
        fprintf(stderr, "Server path is too long.\n");
        return -1;
    }
    command_length = strlen(command);
    if (send_frame(socket_fd, command, command_length) < 0) {
        perror("cannot send download command");
        return -1;
    }
    if (receive_server_frame(socket_fd, &frame, &frame_length) < 0) {
        perror("cannot receive download data");
        return -1;
    }
    if (frame_length >= 6 && memcmp(frame, "ERROR:", 6) == 0) {
        fwrite(frame, 1, frame_length, stderr);
        free(frame);
        return -1;
    }
    if (frame_length != strlen("OK: download begins\n") ||
        memcmp(frame, "OK: download begins\n", frame_length) != 0) {
        fprintf(stderr, "invalid download response from server\n");
        free(frame);
        return -1;
    }
    free(frame);
    frame = NULL;

    file_fd = open(client_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (file_fd < 0) {
        perror("cannot create local download file");
        return -1;
    }
    for (;;) {
        size_t written;

        if (receive_server_frame(socket_fd, &frame, &frame_length) < 0) {
            perror("download connection interrupted");
            break;
        }
        if (frame_length == 0) {
            free(frame);
            frame = NULL;
            result = 0;
            break;
        }
        written = 0;
        while (written < frame_length) {
            ssize_t count;
            count = write(file_fd, frame + written, frame_length - written);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                perror("cannot write local download file");
                free(frame);
                goto done;
            }
            written += (size_t)count;
        }
        free(frame);
        frame = NULL;
    }

done:
    free(frame);
    if (close(file_fd) < 0) {
        perror("cannot close local download file");
        result = -1;
    }
    return result;
}

static int read_remote_file(int socket_fd, const char *command)
{
    unsigned char *frame;
    size_t frame_length;

    if (send_frame(socket_fd, command, strlen(command)) < 0) {
        perror("cannot send read command");
        return -1;
    }
    if (receive_server_frame(socket_fd, &frame, &frame_length) < 0) {
        perror("cannot receive read response");
        return -1;
    }
    if (frame_length >= 6 && memcmp(frame, "ERROR:", 6) == 0) {
        fwrite(frame, 1, frame_length, stderr);
        free(frame);
        return -1;
    }
    if (frame_length != strlen("OK: read begins\n") ||
        memcmp(frame, "OK: read begins\n", frame_length) != 0) {
        fprintf(stderr, "invalid read response from server\n");
        free(frame);
        return -1;
    }
    free(frame);

    for (;;) {
        if (receive_frame(socket_fd, &frame, &frame_length) < 0) {
            perror("read connection interrupted");
            return -1;
        }
        if (frame_length == 0) {
            free(frame);
            return 0;
        }
        if (fwrite(frame, 1, frame_length, stdout) != frame_length) {
            perror("cannot print file content");
            free(frame);
            return -1;
        }
        free(frame);
    }
}

static int upload_file(int socket_fd, const char *client_path,
                       const char *server_path)
{
    char command[PATH_MAX + 32];
    unsigned char buffer[TRANSFER_CHUNK];
    size_t command_length;
    int file_fd;

    file_fd = open(client_path, O_RDONLY | O_NOFOLLOW);
    if (file_fd < 0) {
        perror("cannot open local upload file");
        return -1;
    }
    if (snprintf(command, sizeof(command), "upload %s", server_path) >=
        (int)sizeof(command)) {
        fprintf(stderr, "Server path is too long.\n");
        close(file_fd);
        return -1;
    }
    command_length = strlen(command);
    if (send_frame(socket_fd, command, command_length) < 0) {
        perror("cannot send upload command");
        close(file_fd);
        return -1;
    }
    for (;;) {
        size_t chunk_length;

        if (read_file_chunk(file_fd, buffer, sizeof(buffer), &chunk_length) < 0) {
            perror("cannot read local upload file");
            close(file_fd);
            return -1;
        }
        if (send_frame(socket_fd, buffer, chunk_length) < 0) {
            perror("cannot send upload data");
            close(file_fd);
            return -1;
        }
        if (chunk_length == 0) {
            break;
        }
    }
    close(file_fd);

    {
        unsigned char *response;
        size_t response_length;

        if (receive_frame(socket_fd, &response, &response_length) < 0) {
            perror("cannot receive upload response");
            return -1;
        }
        fwrite(response, 1, response_length, stdout);
        if (response_length >= 6 && memcmp(response, "ERROR:", 6) == 0) {
            free(response);
            return -1;
        }
        free(response);
    }
    return 0;
}

static int perform_transfer(const char *ip, const char *port,
                            const char *username, const char *operation,
                            const char *first_path, const char *second_path)
{
    int socket_fd = connect_to_server(ip, port);
    char login_command[sizeof("login ") + 128];
    int result;

    if (socket_fd < 0) {
        return -1;
    }
    if (snprintf(login_command, sizeof(login_command), "login %s", username) >=
        (int)sizeof(login_command) ||
        send_frame(socket_fd, login_command, strlen(login_command)) < 0) {
        perror("cannot log in for background transfer");
        close(socket_fd);
        return -1;
    }
    {
        unsigned char *response;
        size_t response_length;
        if (receive_frame(socket_fd, &response, &response_length) < 0) {
            perror("cannot receive login response");
            close(socket_fd);
            return -1;
        }
        if (response_length < 2 || memcmp(response, "OK", 2) != 0) {
            fwrite(response, 1, response_length, stderr);
            free(response);
            close(socket_fd);
            return -1;
        }
        free(response);
    }
    if (strcmp(operation, "upload") == 0) {
        result = upload_file(socket_fd, first_path, second_path);
    } else {
        result = download_file(socket_fd, first_path, second_path);
    }
    close(socket_fd);
    return result;
}

static int collect_write_input(unsigned char **contents, size_t *length)
{
    char *line = NULL;
    size_t line_capacity = 0;
    size_t capacity = 0;
    ssize_t line_length;

    *contents = NULL;
    *length = 0;
    printf("Enter file content; type a single '.' on a line to finish.\n");
    while ((line_length = getline(&line, &line_capacity, stdin)) >= 0) {
        if ((line_length == 2 && line[0] == '.' && line[1] == '\n') ||
            (line_length == 1 && line[0] == '.')) {
            free(line);
            return 0;
        }
        if (*length + (size_t)line_length > PROTOCOL_MAX_FRAME) {
            fprintf(stderr, "Input exceeds the %u-byte limit.\n",
                    PROTOCOL_MAX_FRAME);
            free(line);
            free(*contents);
            *contents = NULL;
            *length = 0;
            return -1;
        }
        if (*length + (size_t)line_length > capacity) {
            size_t new_capacity = capacity == 0 ? 1024 : capacity;
            unsigned char *new_contents;
            while (new_capacity < *length + (size_t)line_length) {
                new_capacity *= 2;
            }
            new_contents = realloc(*contents, new_capacity);
            if (new_contents == NULL) {
                free(line);
                free(*contents);
                *contents = NULL;
                *length = 0;
                return -1;
            }
            *contents = new_contents;
            capacity = new_capacity;
        }
        memcpy(*contents + *length, line, (size_t)line_length);
        *length += (size_t)line_length;
    }
    free(line);
    return feof(stdin) ? 0 : -1;
}

static int is_write_command(const char *command)
{
    char *copy = strdup(command);
    char *arguments[4] = {0};
    char *save_pointer = NULL;
    char *token;
    size_t count = 0;
    int valid = 0;

    if (copy == NULL) {
        return 0;
    }
    for (token = strtok_r(copy, " \t", &save_pointer);
         token != NULL && count < 4;
         token = strtok_r(NULL, " \t", &save_pointer)) {
        arguments[count++] = token;
    }
    if (count == 2 && strcmp(arguments[0], "write") == 0) {
        valid = 1;
    } else if (count == 3 && strcmp(arguments[0], "write") == 0 &&
               strncmp(arguments[1], "-offset=", 8) == 0 &&
               arguments[1][8] != '\0') {
        char *end;
        long long offset;

        errno = 0;
        offset = strtoll(arguments[1] + 8, &end, 10);
        valid = errno == 0 && *end == '\0' && offset >= 0;
    }
    free(copy);
    return valid;
}

static int is_read_command(const char *command)
{
    char *copy = strdup(command);
    char *arguments[4] = {0};
    char *save_pointer = NULL;
    char *token;
    size_t count = 0;
    int valid = 0;

    if (copy == NULL) {
        return 0;
    }
    for (token = strtok_r(copy, " \t", &save_pointer);
         token != NULL && count < 4;
         token = strtok_r(NULL, " \t", &save_pointer)) {
        arguments[count++] = token;
    }
    if (count == 2 && strcmp(arguments[0], "read") == 0) {
        valid = 1;
    } else if (count == 3 && strcmp(arguments[0], "read") == 0 &&
               strncmp(arguments[1], "-offset=", 8) == 0 &&
               arguments[1][8] != '\0') {
        char *end;
        long long offset;

        errno = 0;
        offset = strtoll(arguments[1] + 8, &end, 10);
        valid = errno == 0 && *end == '\0' && offset >= 0;
    }
    free(copy);
    return valid;
}

static int parse_transfer_command(const char *command, transfer_command *transfer)
{
    char *copy = strdup(command);
    char *arguments[5] = {0};
    char *save_pointer = NULL;
    char *token;
    size_t count = 0;
    int result = 0;

    if (copy == NULL) {
        return -1;
    }
    for (token = strtok_r(copy, " \t", &save_pointer);
         token != NULL && count < 5;
         token = strtok_r(NULL, " \t", &save_pointer)) {
        arguments[count++] = token;
    }
    if (count > 0 &&
        (strcmp(arguments[0], "upload") == 0 ||
         strcmp(arguments[0], "download") == 0)) {
        if (count == 4 && strcmp(arguments[3], "-b") == 0) {
            transfer->background = 1;
            count = 3;
        } else if (count == 4 && strcmp(arguments[1], "-b") == 0) {
            transfer->background = 1;
            arguments[1] = arguments[2];
            arguments[2] = arguments[3];
            count = 3;
        }
        if (count != 3) {
            result = -1;
        } else {
            snprintf(transfer->operation, sizeof(transfer->operation), "%s",
                     arguments[0]);
            snprintf(transfer->first_path, sizeof(transfer->first_path), "%s",
                     arguments[1]);
            snprintf(transfer->second_path, sizeof(transfer->second_path), "%s",
                     arguments[2]);
            result = 1;
        }
    }
    free(copy);
    return result;
}

static void reap_background_jobs(pid_t *jobs, size_t *job_count)
{
    size_t index = 0;

    while (index < *job_count) {
        pid_t result = waitpid(jobs[index], NULL, WNOHANG);
        if (result == jobs[index] || (result < 0 && errno == ECHILD)) {
            jobs[index] = jobs[--(*job_count)];
        } else {
            ++index;
        }
    }
}

static int start_background_transfer(pid_t *jobs, size_t *job_count,
                                     const char *ip, const char *port,
                                     const char *username,
                                     int control_socket,
                                     const transfer_command *transfer)
{
    pid_t child;

    if (*job_count >= MAX_BACKGROUND_JOBS) {
        fprintf(stderr, "Too many background operations are running.\n");
        return -1;
    }
    child = fork();
    if (child < 0) {
        perror("cannot start background transfer");
        return -1;
    }
    if (child == 0) {
        close(control_socket);
        int result = perform_transfer(ip, port, username, transfer->operation,
                                      transfer->first_path,
                                      transfer->second_path);
        if (result == 0) {
            printf("[Background] Command: %s %s %s concluded\n",
                   transfer->operation,
                   strcmp(transfer->operation, "upload") == 0
                       ? transfer->second_path : transfer->first_path,
                   strcmp(transfer->operation, "upload") == 0
                       ? transfer->first_path : transfer->second_path);
            fflush(stdout);
        }
        _exit(result == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }
    jobs[(*job_count)++] = child;
    printf("Started background %s (pid %ld).\n",
           transfer->operation, (long)child);
    return 0;
}

int main(int argc, char **argv)
{
    const char *ip = argc > 1 ? argv[1] : DEFAULT_IP;
    const char *port = argc > 2 ? argv[2] : DEFAULT_PORT;
    char *line = NULL;
    size_t line_capacity = 0;
    char logged_in_user[128] = {0};
    pid_t background_jobs[MAX_BACKGROUND_JOBS];
    size_t background_job_count = 0;
    int socket_fd;

    if (argc > 3) {
        fprintf(stderr, "Usage: %s [IP] [port]\n", argv[0]);
        return EXIT_FAILURE;
    }
    socket_fd = connect_to_server(ip, port);
    if (socket_fd < 0) {
        return EXIT_FAILURE;
    }
    printf("Connected to %s:%s. Type 'exit' to quit.\n", ip, port);

    for (;;) {
        unsigned char *response;
        size_t response_length;
        ssize_t line_length;
        transfer_command transfer = {0};
        int transfer_result;
        struct pollfd descriptors[2] = {
            { .fd = STDIN_FILENO, .events = POLLIN | POLLHUP },
            { .fd = socket_fd, .events = POLLIN | POLLHUP | POLLERR }
        };

        reap_background_jobs(background_jobs, &background_job_count);
        printf("> ");
        fflush(stdout);
        if (poll(descriptors, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            if (receive_frame(socket_fd, &response, &response_length) < 0) {
                perror("server disconnected");
                break;
            }
            if (response_length >= 8 &&
                memcmp(response, "\001NOTIFY ", 8) == 0) {
                fwrite(response + 8, 1, response_length - 8, stdout);
                fflush(stdout);
                free(response);
                printf("> ");
                fflush(stdout);
                continue;
            }
            fprintf(stderr, "unexpected asynchronous server message\n");
            free(response);
            break;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            fprintf(stderr, "server connection closed\n");
            break;
        }
        if ((descriptors[0].revents & (POLLIN | POLLHUP)) == 0) {
            continue;
        }
        line_length = getline(&line, &line_capacity, stdin);
        if (line_length < 0) {
            while (background_job_count > 0) {
                waitpid(background_jobs[--background_job_count], NULL, 0);
            }
            break;
        }
        while (line_length > 0 &&
               (line[line_length - 1] == '\n' ||
                line[line_length - 1] == '\r')) {
            line[--line_length] = '\0';
        }
        if (line_length == 0) {
            continue;
        }
        transfer_result = parse_transfer_command(line, &transfer);
        if (transfer_result < 0) {
            fprintf(stderr,
                    "Usage: upload <client path> <server path> [-b] or "
                    "download <server path> <client path> [-b]\n");
            continue;
        }
        if (transfer_result > 0) {
            if (logged_in_user[0] == '\0') {
                fprintf(stderr, "Log in before starting a file transfer.\n");
                continue;
            }
            if (transfer.background) {
                start_background_transfer(background_jobs, &background_job_count,
                                          ip, port, logged_in_user, socket_fd,
                                          &transfer);
            } else if (perform_transfer(ip, port, logged_in_user,
                                        transfer.operation,
                                        transfer.first_path,
                                        transfer.second_path) == 0) {
                printf("OK: %s complete\n", transfer.operation);
            }
            continue;
        }
        if ((size_t)line_length > PROTOCOL_MAX_FRAME) {
            fprintf(stderr, "Command is too long.\n");
            continue;
        }
        if (strcmp(line, "exit") == 0) {
            if (background_job_count > 0) {
                printf("%zu background operation(s) still running; "
                       "termination cancelled.\n", background_job_count);
                continue;
            }
            break;
        }
        if (is_read_command(line)) {
            if (read_remote_file(socket_fd, line) < 0) {
                break;
            }
            continue;
        }
        {
            char login_candidate[128] = {0};
            if (sscanf(line, "login %127s", login_candidate) != 1) {
                login_candidate[0] = '\0';
            }
            if (send_frame(socket_fd, line, (size_t)line_length) < 0) {
                perror("cannot send command");
                break;
            }
            if (is_write_command(line)) {
                unsigned char *contents;
                size_t contents_length;

                if (collect_write_input(&contents, &contents_length) < 0) {
                    fprintf(stderr, "Could not collect file content; disconnecting.\n");
                    break;
                }
                if (send_frame(socket_fd, contents, contents_length) < 0) {
                    perror("cannot send file content");
                    free(contents);
                    break;
                }
                free(contents);
            }
            if (receive_server_frame(socket_fd, &response,
                                     &response_length) < 0) {
                perror("server disconnected");
                break;
            }
            if (response_length > 0 &&
                fwrite(response, 1, response_length, stdout) != response_length) {
                perror("cannot print server response");
                free(response);
                break;
            }
            if (login_candidate[0] != '\0' && response_length >= 2 &&
                memcmp(response, "OK", 2) == 0) {
                snprintf(logged_in_user, sizeof(logged_in_user), "%s",
                         login_candidate);
            }
            free(response);
        }
    }

    free(line);
    close(socket_fd);
    return EXIT_SUCCESS;
}
