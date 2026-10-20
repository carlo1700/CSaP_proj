#define _POSIX_C_SOURCE 200809L

#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int transfer_all(int socket_fd, void *buffer, size_t length, int sending)
{
    unsigned char *position = buffer;
    size_t transferred = 0;

    while (transferred < length) {
        ssize_t result;

        if (sending) {
            result = send(socket_fd, position + transferred,
                          length - transferred, MSG_NOSIGNAL);
        } else {
            result = recv(socket_fd, position + transferred,
                          length - transferred, 0);
        }

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result == 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (result < 0) {
            return -1;
        }
        transferred += (size_t)result;
    }

    return 0;
}

int send_frame(int socket_fd, const void *data, size_t length)
{
    uint32_t network_length;

    if (length > NETWORK_MAX_FRAME) {
        errno = EMSGSIZE;
        return -1;
    }

    network_length = htonl((uint32_t)length);
    if (transfer_all(socket_fd, &network_length, sizeof(network_length), 1) < 0) {
        return -1;
    }

    if (length > 0 &&
        transfer_all(socket_fd, (void *)data, length, 1) < 0) {
        return -1;
    }

    return 0;
}

int receive_frame(int socket_fd, unsigned char **data, size_t *length)
{
    uint32_t network_length;
    uint32_t frame_length;
    unsigned char *frame;

    if (transfer_all(socket_fd, &network_length, sizeof(network_length), 0) < 0) {
        return -1;
    }

    frame_length = ntohl(network_length);
    if (frame_length > NETWORK_MAX_FRAME) {
        errno = EMSGSIZE;
        return -1;
    }

    frame = malloc((size_t)frame_length + 1);
    if (frame == NULL) {
        return -1;
    }
    if (frame_length > 0 &&
        transfer_all(socket_fd, frame, frame_length, 0) < 0) {
        free(frame);
        return -1;
    }

    frame[frame_length] = '\0';
    *data = frame;
    *length = frame_length;
    return 0;
}

int send_text(int socket_fd, const char *text)
{
    return send_frame(socket_fd, text, strlen(text));
}

int send_error(int socket_fd, const char *message)
{
    char response[1024];

    snprintf(response, sizeof(response), "ERROR: %s\n", message);
    return send_text(socket_fd, response);
}

/* Returns a listening socket (server) or a connected one (client). */
static int open_socket(const char *ip, const char *port, int listening,
                       char *error, size_t error_size)
{
    struct addrinfo hints = {0};
    struct addrinfo *addresses;
    struct addrinfo *address;
    int socket_fd = -1;
    int option = 1;
    int saved_errno = 0;
    int result;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = listening ? AI_NUMERICHOST : 0;
    result = getaddrinfo(ip, port, &hints, &addresses);
    if (result != 0) {
        snprintf(error, error_size, "invalid address %s:%s: %s",
                 ip, port, gai_strerror(result));
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype,
                           address->ai_protocol);
        if (socket_fd < 0) {
            saved_errno = errno;
            continue;
        }
        if (listening) {
            setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR,
                       &option, sizeof(option));
            result = bind(socket_fd, address->ai_addr, address->ai_addrlen) == 0 &&
                     listen(socket_fd, 32) == 0 ? 0 : -1;
        } else {
            result = connect(socket_fd, address->ai_addr, address->ai_addrlen);
        }
        if (result == 0) {
            break;
        }
        saved_errno = errno;
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(addresses);
    if (socket_fd < 0) {
        snprintf(error, error_size, "cannot %s %s:%s: %s",
                 listening ? "listen on" : "connect to", ip, port,
                 strerror(saved_errno));
    }
    return socket_fd;
}

int network_listen(const char *ip, const char *port,
                   char *error, size_t error_size)
{
    return open_socket(ip, port, 1, error, error_size);
}

int network_connect(const char *ip, const char *port,
                    char *error, size_t error_size)
{
    return open_socket(ip, port, 0, error, error_size);
}
