#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

#define PROTOCOL_MAX_FRAME (1024U * 1024U)

int send_frame(int socket_fd, const void *data, size_t length);
int receive_frame(int socket_fd, unsigned char **data, size_t *length);

#endif
