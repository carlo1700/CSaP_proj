#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "transfer.h"
#include "network.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* POSIX message queues are limited to 8 KiB messages by default. */
#define TRANSFER_CHUNK 4096U
#define TRANSFER_QUEUE_PREFIX "/csap_user_"
#define TRANSFER_REPLY_PREFIX "/csap_reply_"
#define TRANSFER_DATA_PREFIX "/csap_data_"
/* Returned by the sender helpers when the requesting client has disconnected. */
#define CLIENT_GONE (-2)
/* A transfer peer silent for this long is considered gone. */
#define TRANSFER_STALL_SECONDS 30

enum transfer_message_type {
    TRANSFER_REQUEST = 1,
    TRANSFER_ACCEPT,
    TRANSFER_REJECT,
    TRANSFER_DATA,
    TRANSFER_END,
    TRANSFER_RESULT,
    TRANSFER_ERROR
};

/* Sent by the sender's handler to the recipient's inbox. */
typedef struct {
    int type;
    pid_t sender_pid;
    uid_t sender_uid;
    char request_id[64];
    char sender[128];
    char filename[NAME_MAX + 1];
    char reply_queue[64];
    char data_queue[64];
} transfer_request_message;

/* Decisions and results travel on the reply queue, file data on the data queue. */
typedef struct {
    int type;
    union {
        struct {
            uint32_t length;
            unsigned char data[TRANSFER_CHUNK];
        } chunk;
        char error[256];
    } payload;
} transfer_reply_message;

struct pending_transfer {
    transfer_request_message request;
    struct pending_transfer *next;
};

static unsigned long long next_transfer_id;

static int make_queue_name(char *buffer, size_t buffer_size,
                           const char *prefix, unsigned long long identifier)
{
    int written = snprintf(buffer, buffer_size, "%s%llu",
                           prefix, identifier);
    if (written < 0 || (size_t)written >= buffer_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

mqd_t transfer_open_inbox(uid_t uid)
{
    char name[64];
    struct mq_attr attributes = {0};
    mode_t old_mask;
    mqd_t queue;

    if (make_queue_name(name, sizeof(name), TRANSFER_QUEUE_PREFIX,
                        (unsigned long long)uid) < 0) {
        return (mqd_t)-1;
    }
    attributes.mq_maxmsg = 10;
    attributes.mq_msgsize = (long)sizeof(transfer_request_message);
    old_mask = umask(0007);
    queue = mq_open(name, O_CREAT | O_RDONLY | O_NONBLOCK, 0620, &attributes);
    umask(old_mask);
    return queue;
}

/* ---------- sender side ---------- */

static int client_disconnected(int client_fd)
{
    char byte;
    return recv(client_fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

static int create_transfer_queues(transfer_request_message *request,
                                  mqd_t *reply_queue, mqd_t *data_queue)
{
    struct mq_attr attributes = {0};
    mode_t old_mask = umask(0007);
    unsigned long long identifier;

    attributes.mq_maxmsg = 10;
    attributes.mq_msgsize = (long)sizeof(transfer_reply_message);
    do {
        identifier = ((unsigned long long)getpid() << 32) ^
                     (unsigned long long)time(NULL) ^ ++next_transfer_id;
        snprintf(request->reply_queue, sizeof(request->reply_queue),
                 "%s%ld_%llu", TRANSFER_REPLY_PREFIX, (long)getpid(), identifier);
        *reply_queue = mq_open(request->reply_queue, O_CREAT | O_EXCL | O_RDWR,
                               0620, &attributes);
    } while (*reply_queue == (mqd_t)-1 && errno == EEXIST);
    if (*reply_queue != (mqd_t)-1) {
        snprintf(request->data_queue, sizeof(request->data_queue),
                 "%s%ld_%llu", TRANSFER_DATA_PREFIX, (long)getpid(), identifier);
        *data_queue = mq_open(request->data_queue, O_CREAT | O_EXCL | O_WRONLY,
                              0640, &attributes);
    }
    umask(old_mask);
    return *reply_queue == (mqd_t)-1 || *data_queue == (mqd_t)-1 ? -1 : 0;
}

static void remove_transfer_queues(const transfer_request_message *request,
                                   mqd_t reply_queue, mqd_t data_queue)
{
    if (reply_queue != (mqd_t)-1) {
        mq_close(reply_queue);
        mq_unlink(request->reply_queue);
    }
    if (data_queue != (mqd_t)-1) {
        mq_close(data_queue);
        mq_unlink(request->data_queue);
    }
}

/* Queues the request; while the recipient has no inbox yet, keep waiting. */
static int deliver_request(const transfer_request_message *request,
                           uid_t recipient_uid, int client_fd)
{
    char name[64];

    if (make_queue_name(name, sizeof(name), TRANSFER_QUEUE_PREFIX,
                        (unsigned long long)recipient_uid) < 0) {
        return -1;
    }
    for (;;) {
        struct timespec pause = {0, 250000000};
        mqd_t queue = mq_open(name, O_WRONLY | O_NONBLOCK);

        if (queue != (mqd_t)-1) {
            int sent = mq_send(queue, (const char *)request, sizeof(*request), 0);
            int saved_errno = errno;

            mq_close(queue);
            if (sent == 0) {
                return 0;
            }
            if (saved_errno != EAGAIN && saved_errno != EINTR) {
                errno = saved_errno;
                return -1;
            }
        } else if (errno != ENOENT) {
            return -1;
        }
        nanosleep(&pause, NULL);
        if (client_disconnected(client_fd)) {
            return CLIENT_GONE;
        }
    }
}

static int wait_reply(mqd_t queue, int client_fd, transfer_reply_message *reply)
{
    for (;;) {
        struct timespec deadline;
        ssize_t received;

        clock_gettime(CLOCK_REALTIME, &deadline);
        ++deadline.tv_sec;
        received = mq_timedreceive(queue, (char *)reply, sizeof(*reply),
                                   NULL, &deadline);
        if (received == (ssize_t)sizeof(*reply)) {
            return 0;
        }
        if (received >= 0) {
            errno = EBADMSG;
            return -1;
        }
        if (errno != EINTR && errno != ETIMEDOUT) {
            return -1;
        }
        if (client_disconnected(client_fd)) {
            return CLIENT_GONE;
        }
    }
}

static int send_chunk(mqd_t data_queue, const transfer_reply_message *chunk,
                      int client_fd)
{
    int stalled_seconds = 0;

    for (;;) {
        struct timespec deadline;

        clock_gettime(CLOCK_REALTIME, &deadline);
        ++deadline.tv_sec;
        if (mq_timedsend(data_queue, (const char *)chunk, sizeof(*chunk), 0,
                         &deadline) == 0) {
            return 0;
        }
        if ((errno != ETIMEDOUT && errno != EINTR) ||
            ++stalled_seconds >= TRANSFER_STALL_SECONDS) {
            return -1;
        }
        if (client_disconnected(client_fd)) {
            return CLIENT_GONE;
        }
    }
}

static int stream_source(int source_fd, mqd_t data_queue, int client_fd,
                         char *error, size_t error_size)
{
    for (;;) {
        transfer_reply_message chunk = {0};
        ssize_t count = read_retry(source_fd, chunk.payload.chunk.data,
                                   TRANSFER_CHUNK);
        int status;

        if (count < 0) {
            snprintf(error, error_size, "cannot read source file");
            chunk.type = TRANSFER_ERROR;
            snprintf(chunk.payload.error, sizeof(chunk.payload.error), "%s", error);
            send_chunk(data_queue, &chunk, client_fd);
            return -1;
        }
        chunk.type = count == 0 ? TRANSFER_END : TRANSFER_DATA;
        chunk.payload.chunk.length = (uint32_t)count;
        status = send_chunk(data_queue, &chunk, client_fd);
        if (status == -1) {
            snprintf(error, error_size, "recipient stopped receiving the file");
        }
        if (status != 0 || count == 0) {
            return status;
        }
    }
}

int transfer_request(int client_fd, const fs_session *session,
                     const char *path, const char *recipient)
{
    struct passwd *recipient_user = getpwnam(recipient);
    transfer_request_message request = {0};
    transfer_reply_message reply = {0};
    struct timespec now;
    char error[512] = "";
    mqd_t reply_queue = (mqd_t)-1;
    mqd_t data_queue = (mqd_t)-1;
    uid_t recipient_uid;
    int source_fd;
    int status;

    if (recipient_user == NULL || recipient_user->pw_uid == session->uid) {
        return send_error(client_fd, "recipient must be a different existing user");
    }
    recipient_uid = recipient_user->pw_uid;
    if (fs_transfer_source(session, path, &source_fd, request.filename,
                           sizeof(request.filename), error, sizeof(error)) < 0) {
        return send_error(client_fd, error);
    }

    status = create_transfer_queues(&request, &reply_queue, &data_queue);
    if (status == 0) {
        request.type = TRANSFER_REQUEST;
        request.sender_pid = getpid();
        request.sender_uid = session->uid;
        snprintf(request.sender, sizeof(request.sender), "%s", session->username);
        clock_gettime(CLOCK_REALTIME, &now);
        snprintf(request.request_id, sizeof(request.request_id), "%ld-%lld-%llu",
                 (long)getpid(), (long long)now.tv_sec,
                 (unsigned long long)now.tv_nsec ^ next_transfer_id);
        status = deliver_request(&request, recipient_uid, client_fd);
    }
    if (status == 0) {
        status = wait_reply(reply_queue, client_fd, &reply);
    }
    if (status == 0 && reply.type != TRANSFER_ACCEPT) {
        if (reply.type == TRANSFER_REJECT) {
            snprintf(error, sizeof(error), "recipient rejected the transfer request");
        } else if (reply.type == TRANSFER_ERROR) {
            snprintf(error, sizeof(error), "recipient error: %.255s",
                     reply.payload.error);
        } else {
            snprintf(error, sizeof(error), "invalid transfer decision");
        }
        status = -1;
    }
    if (status == 0) {
        status = stream_source(source_fd, data_queue, client_fd,
                               error, sizeof(error));
    }
    if (status == 0) {
        status = wait_reply(reply_queue, client_fd, &reply);
    }
    if (status == 0 &&
        (reply.type != TRANSFER_RESULT || reply.payload.error[0] != '\0')) {
        snprintf(error, sizeof(error), "%.255s",
                 reply.type == TRANSFER_RESULT ? reply.payload.error
                                               : "invalid transfer result");
        status = -1;
    }
    if (status == -1 && error[0] == '\0') {
        snprintf(error, sizeof(error), "transfer request failed: %s",
                 strerror(errno));
    }

    close(source_fd);
    remove_transfer_queues(&request, reply_queue, data_queue);
    if (status == CLIENT_GONE) {
        return -1;
    }
    if (status < 0) {
        return send_error(client_fd, error);
    }
    return send_text(client_fd, "OK: transfer request accepted; file copied\n");
}

/* ---------- recipient side ---------- */

/* Rejects forged or stale requests: the queues must belong to a live sender. */
static int valid_pending_request(const transfer_request_message *request)
{
    struct passwd *sender;
    char queue_path[128];
    struct stat status;
    int written;
    long queue_pid;
    unsigned long long queue_nonce;
    long data_pid;
    unsigned long long data_nonce;
    char trailing_character;

    if (request->type != TRANSFER_REQUEST ||
        memchr(request->request_id, '\0', sizeof(request->request_id)) == NULL ||
        memchr(request->sender, '\0', sizeof(request->sender)) == NULL ||
        memchr(request->filename, '\0', sizeof(request->filename)) == NULL ||
        memchr(request->reply_queue, '\0', sizeof(request->reply_queue)) == NULL ||
        memchr(request->data_queue, '\0', sizeof(request->data_queue)) == NULL ||
        request->request_id[0] == '\0' ||
        request->sender[0] == '\0' ||
        request->filename[0] == '\0' ||
        strncmp(request->reply_queue, TRANSFER_REPLY_PREFIX,
                strlen(TRANSFER_REPLY_PREFIX)) != 0 ||
        strncmp(request->data_queue, TRANSFER_DATA_PREFIX,
                strlen(TRANSFER_DATA_PREFIX)) != 0 ||
        request->sender_pid <= 0) {
        return 0;
    }
    if (kill(request->sender_pid, 0) < 0 && errno == ESRCH) {
        return 0;
    }
    if (sscanf(request->reply_queue, TRANSFER_REPLY_PREFIX "%ld_%llu%c",
               &queue_pid, &queue_nonce, &trailing_character) != 2 ||
        queue_pid != (long)request->sender_pid ||
        sscanf(request->data_queue, TRANSFER_DATA_PREFIX "%ld_%llu%c",
               &data_pid, &data_nonce, &trailing_character) != 2 ||
        data_pid != (long)request->sender_pid ||
        data_nonce != queue_nonce) {
        return 0;
    }
    sender = getpwnam(request->sender);
    if (sender == NULL || sender->pw_uid != request->sender_uid) {
        return 0;
    }
    written = snprintf(queue_path, sizeof(queue_path), "/dev/mqueue/%s",
                       request->reply_queue + 1);
    if (written < 0 || (size_t)written >= sizeof(queue_path) ||
        stat(queue_path, &status) < 0 || status.st_uid != request->sender_uid) {
        return 0;
    }
    written = snprintf(queue_path, sizeof(queue_path), "/dev/mqueue/%s",
                       request->data_queue + 1);
    if (written < 0 || (size_t)written >= sizeof(queue_path) ||
        stat(queue_path, &status) < 0 || status.st_uid != request->sender_uid) {
        return 0;
    }
    return 1;
}

static int send_decision(const transfer_request_message *request, int type)
{
    transfer_reply_message reply = {0};
    mqd_t queue = mq_open(request->reply_queue, O_WRONLY | O_NONBLOCK);
    int result;

    if (queue == (mqd_t)-1) {
        return -1;
    }
    reply.type = type;
    result = mq_send(queue, (const char *)&reply, sizeof(reply), 0);
    mq_close(queue);
    return result;
}

int transfer_notify(mqd_t inbox, int client_fd, pending_transfer **pending)
{
    transfer_request_message request;
    ssize_t received = mq_receive(inbox, (char *)&request, sizeof(request), NULL);
    pending_transfer *entry;
    char notification[1024];

    if (received < 0) {
        return errno == EAGAIN || errno == EINTR ? 0 : -1;
    }
    if ((size_t)received != sizeof(request) || !valid_pending_request(&request)) {
        return 0;
    }
    for (entry = *pending; entry != NULL; entry = entry->next) {
        if (strcmp(entry->request.request_id, request.request_id) == 0) {
            return 0;
        }
    }
    entry = malloc(sizeof(*entry));
    if (entry == NULL) {
        return -1;
    }
    entry->request = request;
    entry->next = *pending;
    *pending = entry;

    snprintf(notification, sizeof(notification),
             "%sTransfer request %s from %s for file %s. "
             "Use: accept <directory> %s  |  reject %s\n",
             NETWORK_NOTIFY_PREFIX, request.request_id, request.sender,
             request.filename, request.request_id, request.request_id);
    return send_text(client_fd, notification);
}

static int receive_accepted_file(int client_fd, const fs_session *session,
                                 const transfer_request_message *request,
                                 const char *directory)
{
    transfer_reply_message reply = {0};
    char target_path[PATH_MAX];
    char error[512];
    mqd_t sender_queue;
    mqd_t data_queue;
    int target_fd;
    int failed = 0;
    int idle_seconds = 0;

    if (!valid_pending_request(request)) {
        return send_error(client_fd, "sender is no longer online");
    }
    sender_queue = mq_open(request->reply_queue, O_WRONLY);
    if (sender_queue == (mqd_t)-1) {
        return send_error(client_fd, "cannot contact transfer sender");
    }
    data_queue = mq_open(request->data_queue, O_RDONLY);
    if (data_queue == (mqd_t)-1) {
        mq_close(sender_queue);
        return send_error(client_fd, "cannot open transfer data channel");
    }
    if (fs_transfer_target(session, directory, request->filename,
                           &target_fd, target_path, sizeof(target_path),
                           error, sizeof(error)) < 0) {
        reply.type = TRANSFER_ERROR;
        snprintf(reply.payload.error, sizeof(reply.payload.error), "%.255s", error);
        mq_send(sender_queue, (const char *)&reply, sizeof(reply), 0);
        mq_close(sender_queue);
        mq_close(data_queue);
        return send_error(client_fd, error);
    }

    reply.type = TRANSFER_ACCEPT;
    if (mq_send(sender_queue, (const char *)&reply, sizeof(reply), 0) < 0) {
        fs_transfer_finish(target_fd, target_path, 0, error, sizeof(error));
        mq_close(sender_queue);
        mq_close(data_queue);
        return send_error(client_fd, "cannot notify transfer sender");
    }

    for (;;) {
        struct timespec deadline;
        ssize_t received;

        clock_gettime(CLOCK_REALTIME, &deadline);
        ++deadline.tv_sec;
        received = mq_timedreceive(data_queue, (char *)&reply,
                                   sizeof(reply), NULL, &deadline);
        if (received < 0 && (errno == EINTR || errno == ETIMEDOUT)) {
            if ((kill(request->sender_pid, 0) < 0 && errno == ESRCH) ||
                ++idle_seconds >= TRANSFER_STALL_SECONDS) {
                failed = 1;
                snprintf(error, sizeof(error), "transfer sender disconnected");
                break;
            }
            continue;
        }
        idle_seconds = 0;
        if (received != (ssize_t)sizeof(reply)) {
            failed = 1;
            snprintf(error, sizeof(error), "cannot receive transfer data");
            break;
        }
        if (reply.type == TRANSFER_END) {
            break;
        }
        if (reply.type == TRANSFER_ERROR) {
            failed = 1;
            snprintf(error, sizeof(error), "%.255s", reply.payload.error);
            break;
        }
        if (reply.type != TRANSFER_DATA ||
            reply.payload.chunk.length > TRANSFER_CHUNK) {
            failed = 1;
            snprintf(error, sizeof(error), "invalid transfer data");
            break;
        }
        if (!failed &&
            write_all(target_fd, reply.payload.chunk.data,
                      reply.payload.chunk.length) < 0) {
            failed = 1;
            snprintf(error, sizeof(error), "cannot write transfer destination");
        }
    }
    if (fs_transfer_finish(target_fd, target_path, !failed,
                           error, sizeof(error)) < 0) {
        failed = 1;
    }
    reply = (transfer_reply_message){0};
    reply.type = TRANSFER_RESULT;
    if (failed) {
        snprintf(reply.payload.error, sizeof(reply.payload.error), "%.255s", error);
    }
    if (mq_send(sender_queue, (const char *)&reply, sizeof(reply), 0) < 0 &&
        !failed) {
        failed = 1;
        snprintf(error, sizeof(error), "cannot return transfer result to sender");
    }
    mq_close(sender_queue);
    mq_close(data_queue);
    if (failed) {
        return send_error(client_fd, error);
    }
    return send_text(client_fd, "OK: transfer accepted and file copied\n");
}

int transfer_decide(int client_fd, const fs_session *session,
                    pending_transfer **pending, char **arguments,
                    size_t argument_count)
{
    int accept = strcmp(arguments[0], "accept") == 0;
    pending_transfer **position = pending;
    pending_transfer *entry;
    const char *request_id;
    int result;

    if (argument_count != (accept ? 3U : 2U)) {
        return send_error(client_fd, accept ? "usage: accept <directory> <ID>"
                                            : "usage: reject <ID>");
    }
    request_id = arguments[argument_count - 1];
    while (*position != NULL &&
           strcmp((*position)->request.request_id, request_id) != 0) {
        position = &(*position)->next;
    }
    if (*position == NULL) {
        return send_error(client_fd, "unknown or expired transfer request ID");
    }
    entry = *position;
    *position = entry->next;
    if (accept) {
        result = receive_accepted_file(client_fd, session, &entry->request,
                                       arguments[1]);
    } else if (send_decision(&entry->request, TRANSFER_REJECT) < 0) {
        result = send_error(client_fd, "cannot notify transfer sender");
    } else {
        result = send_text(client_fd, "OK: transfer request rejected\n");
    }
    free(entry);
    return result;
}

void transfer_discard_pending(pending_transfer *pending)
{
    while (pending != NULL) {
        pending_transfer *next = pending->next;

        send_decision(&pending->request, TRANSFER_REJECT);
        free(pending);
        pending = next;
    }
}
