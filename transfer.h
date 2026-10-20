#ifndef TRANSFER_H
#define TRANSFER_H

#include "filesystem.h"

#include <mqueue.h>
#include <stddef.h>
#include <sys/types.h>

/* Requests received by a logged-in user and not yet accepted or rejected. */
typedef struct pending_transfer pending_transfer;

/*
 * All functions answer the client themselves and return 0, or -1 when the
 * client connection is broken.
 */

/* Per-user queue where transfer requests are delivered (created if missing). */
mqd_t transfer_open_inbox(uid_t uid);
/* Sender side: blocks until the recipient accepts or rejects the request. */
int transfer_request(int client_fd, const fs_session *session,
                     const char *path, const char *recipient);
/* Recipient side: reads a request from the inbox and notifies the client. */
int transfer_notify(mqd_t inbox, int client_fd, pending_transfer **pending);
/* Recipient side: "accept <directory> <ID>" or "reject <ID>". */
int transfer_decide(int client_fd, const fs_session *session,
                    pending_transfer **pending, char **arguments,
                    size_t argument_count);
/* Rejects and frees the requests left when the recipient disconnects. */
void transfer_discard_pending(pending_transfer *pending);

#endif
