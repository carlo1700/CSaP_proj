#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_IP "127.0.0.1"
#define DEFAULT_PORT "8080"

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

int main(int argc, char **argv)
{
    const char *ip = argc > 1 ? argv[1] : DEFAULT_IP;
    const char *port = argc > 2 ? argv[2] : DEFAULT_PORT;
    char *line = NULL;
    size_t line_capacity = 0;
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

        printf("> ");
        fflush(stdout);
        line_length = getline(&line, &line_capacity, stdin);
        if (line_length < 0) {
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
        if ((size_t)line_length > PROTOCOL_MAX_FRAME) {
            fprintf(stderr, "Command is too long.\n");
            continue;
        }
        if (strcmp(line, "exit") == 0) {
            break;
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
        if (receive_frame(socket_fd, &response, &response_length) < 0) {
            perror("server disconnected");
            break;
        }
        if (response_length > 0 &&
            fwrite(response, 1, response_length, stdout) != response_length) {
            perror("cannot print server response");
            free(response);
            break;
        }
        free(response);
    }

    free(line);
    close(socket_fd);
    return EXIT_SUCCESS;
}
