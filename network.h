#ifndef NETWORK_H
#define NETWORK_H

#include <stddef.h>

#define NETWORK_MAX_FRAME (1024U * 1024U)
/* Size of the data frames used to stream files. */
#define NETWORK_CHUNK (64U * 1024U)
/* Prefix of asynchronous server messages (transfer notifications). */
#define NETWORK_NOTIFY_PREFIX "\001NOTIFY "

/* Every message is a 4-byte big-endian length followed by the payload. */
int send_frame(int socket_fd, const void *data, size_t length);
int receive_frame(int socket_fd, unsigned char **data, size_t *length);
int send_text(int socket_fd, const char *text);
int send_error(int socket_fd, const char *message);

int network_listen(const char *ip, const char *port,
                   char *error, size_t error_size);
int network_connect(const char *ip, const char *port,
                    char *error, size_t error_size);

#endif
