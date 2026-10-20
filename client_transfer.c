#define _POSIX_C_SOURCE 200809L

#include "client_transfer.h"
#include "network.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int receive_response(int socket_fd, unsigned char **data, size_t *length)
{
    const size_t prefix_length = strlen(NETWORK_NOTIFY_PREFIX);

    for (;;) {
        if (receive_frame(socket_fd, data, length) < 0) {
            return -1;
        }
        if (*length < prefix_length ||
            memcmp(*data, NETWORK_NOTIFY_PREFIX, prefix_length) != 0) {
            return 0;
        }
        fwrite(*data + prefix_length, 1, *length - prefix_length, stdout);
        fflush(stdout);
        free(*data);
    }
}

/* Receives one answer and prints it (always if verbose, otherwise on error). */
static int check_response(int socket_fd, int verbose)
{
    unsigned char *response;
    size_t length;
    int ok;

    if (receive_response(socket_fd, &response, &length) < 0) {
        print_error("connection to server lost");
        return -1;
    }
    ok = length >= 2 && memcmp(response, "OK", 2) == 0;
    if (verbose || !ok) {
        fwrite(response, 1, length, stdout);
    }
    free(response);
    return ok ? 0 : -1;
}

static int expect_ok(int socket_fd, const char *command)
{
    if (send_text(socket_fd, command) < 0) {
        print_error("cannot send command");
        return -1;
    }
    return check_response(socket_fd, 0);
}

/*
 * Receives "<banner>" and then data frames up to an empty one, writing them
 * to path (stdout if NULL). Returns 1 if the server refused the request.
 */
static int receive_stream(int socket_fd, const char *banner, const char *path)
{
    unsigned char *frame;
    size_t frame_length;
    int output_fd = STDOUT_FILENO;
    int result = 0;

    if (receive_response(socket_fd, &frame, &frame_length) < 0) {
        print_error("connection to server lost");
        return -1;
    }
    if (frame_length != strlen(banner) ||
        memcmp(frame, banner, frame_length) != 0) {
        int refused = frame_length >= 6 && memcmp(frame, "ERROR:", 6) == 0;

        if (refused) {
            fwrite(frame, 1, frame_length, stdout);
        } else {
            printf("ERROR: unexpected response from server\n");
        }
        free(frame);
        return refused ? 1 : -1;
    }
    free(frame);

    if (path != NULL) {
        output_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (output_fd < 0) {
            print_error("cannot create local file");
            return -1;
        }
    } else {
        fflush(stdout);
    }
    for (;;) {
        int failed;

        if (receive_frame(socket_fd, &frame, &frame_length) < 0) {
            print_error("transfer interrupted");
            result = -1;
            break;
        }
        if (frame_length == 0) {
            free(frame);
            break;
        }
        failed = write_all(output_fd, frame, frame_length);
        free(frame);
        if (failed < 0) {
            print_error("cannot write received data");
            result = -1;
            break;
        }
    }
    if (path != NULL && close(output_fd) < 0 && result == 0) {
        print_error("cannot close local file");
        result = -1;
    }
    return result;
}

int read_remote_file(int socket_fd, const char *command)
{
    if (send_text(socket_fd, command) < 0) {
        print_error("cannot send read command");
        return -1;
    }
    return receive_stream(socket_fd, "OK: read begins\n", NULL);
}

static int download_file(int socket_fd, const char *server_path,
                         const char *client_path)
{
    char command[PATH_MAX + 32];

    snprintf(command, sizeof(command), "download %s", server_path);
    if (send_text(socket_fd, command) < 0) {
        print_error("cannot send download command");
        return -1;
    }
    return receive_stream(socket_fd, "OK: download begins\n", client_path) == 0
               ? 0 : -1;
}

static int upload_file(int socket_fd, const char *client_path,
                       const char *server_path)
{
    char command[PATH_MAX + 32];
    unsigned char buffer[NETWORK_CHUNK];
    int file_fd = open(client_path, O_RDONLY | O_NOFOLLOW);
    ssize_t count;

    if (file_fd < 0) {
        print_error("cannot open local upload file");
        return -1;
    }
    snprintf(command, sizeof(command), "upload %s", server_path);
    if (send_text(socket_fd, command) < 0) {
        print_error("cannot send upload command");
        close(file_fd);
        return -1;
    }
    /* The last, empty frame tells the server that the file is complete. */
    do {
        count = read_retry(file_fd, buffer, sizeof(buffer));
        if (count < 0) {
            print_error("cannot read local upload file");
            close(file_fd);
            return -1;
        }
        if (send_frame(socket_fd, buffer, (size_t)count) < 0) {
            print_error("cannot send upload data");
            close(file_fd);
            return -1;
        }
    } while (count > 0);
    close(file_fd);
    return check_response(socket_fd, 1);
}

int query_cwd(int socket_fd, char *cwd, size_t cwd_size)
{
    unsigned char *response;
    size_t response_length;

    if (send_text(socket_fd, "pwd") < 0 ||
        receive_response(socket_fd, &response, &response_length) < 0) {
        return -1;
    }
    if (response_length < 2 || response_length > cwd_size ||
        response[0] != '/' || response[response_length - 1] != '\n') {
        free(response);
        errno = EPROTO;
        return -1;
    }
    memcpy(cwd, response, response_length - 1);
    cwd[response_length - 1] = '\0';
    free(response);
    return 0;
}

int perform_transfer(const char *ip, const char *port, const char *username,
                     const transfer_command *transfer)
{
    char command[PATH_MAX + 32];
    char error[512];
    int socket_fd = network_connect(ip, port, error, sizeof(error));
    int result = -1;

    if (socket_fd < 0) {
        printf("ERROR: %s\n", error);
        return -1;
    }
    snprintf(command, sizeof(command), "login %s --no-notify", username);
    if (expect_ok(socket_fd, command) == 0) {
        snprintf(command, sizeof(command), "cd %s", transfer->cwd);
        if (expect_ok(socket_fd, command) == 0) {
            result = strcmp(transfer->operation, "upload") == 0
                         ? upload_file(socket_fd, transfer->first_path,
                                       transfer->second_path)
                         : download_file(socket_fd, transfer->first_path,
                                         transfer->second_path);
        }
    }
    close(socket_fd);
    return result;
}

int parse_transfer_command(const char *line, transfer_command *transfer)
{
    char *copy = strdup(line);
    char *arguments[5];
    size_t count;
    int result = 0;

    if (copy == NULL) {
        return -1;
    }
    count = split_arguments(copy, arguments, 5);
    if (count > 0 &&
        (strcmp(arguments[0], "upload") == 0 ||
         strcmp(arguments[0], "download") == 0)) {
        /* -b may come before or after the two paths. */
        if (count == 4 && strcmp(arguments[3], "-b") == 0) {
            transfer->background = 1;
            count = 3;
        } else if (count == 4 && strcmp(arguments[1], "-b") == 0) {
            transfer->background = 1;
            arguments[1] = arguments[2];
            arguments[2] = arguments[3];
            count = 3;
        }
        if (count != 3 || strcmp(arguments[1], "-b") == 0 ||
            strcmp(arguments[2], "-b") == 0) {
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

void reap_background_jobs(background_jobs *jobs)
{
    size_t index = 0;

    while (index < jobs->count) {
        pid_t result = waitpid(jobs->pids[index], NULL, WNOHANG);
        if (result == jobs->pids[index] || (result < 0 && errno == ECHILD)) {
            jobs->pids[index] = jobs->pids[--jobs->count];
        } else {
            ++index;
        }
    }
}

void wait_background_jobs(background_jobs *jobs)
{
    while (jobs->count > 0) {
        waitpid(jobs->pids[--jobs->count], NULL, 0);
    }
}

int start_background_transfer(background_jobs *jobs, const char *ip,
                              const char *port, const char *username,
                              int control_socket,
                              const transfer_command *transfer)
{
    int upload = strcmp(transfer->operation, "upload") == 0;
    pid_t child;

    if (jobs->count >= MAX_BACKGROUND_JOBS) {
        printf("ERROR: too many background operations are running\n");
        return -1;
    }
    /* Otherwise the child would print again what is still buffered. */
    fflush(stdout);
    child = fork();
    if (child < 0) {
        print_error("cannot start background transfer");
        return -1;
    }
    if (child == 0) {
        int result;

        close(control_socket);
        result = perform_transfer(ip, port, username, transfer);
        if (result == 0) {
            printf("[Background] Command: %s %s %s concluded\n",
                   transfer->operation,
                   upload ? transfer->second_path : transfer->first_path,
                   upload ? transfer->first_path : transfer->second_path);
        }
        fflush(stdout);
        _exit(result == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }
    jobs->pids[jobs->count++] = child;
    printf("Started background %s (pid %ld).\n",
           transfer->operation, (long)child);
    return 0;
}
