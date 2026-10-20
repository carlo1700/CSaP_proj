#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>

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
        if (result <= 0) {
            return -1;
        }
        transferred += (size_t)result;
    }

    return 0;
}

int send_frame(int socket_fd, const void *data, size_t length)
{
    uint32_t network_length;

    if (length > PROTOCOL_MAX_FRAME) {
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
    if (frame_length > PROTOCOL_MAX_FRAME) {
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
