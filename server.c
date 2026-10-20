#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "filesystem.h"
#include "privilege.h"
#include "protocol.h"
#include "transfer.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <mqueue.h>
#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_IP "127.0.0.1"
#define DEFAULT_PORT "8080"
#define COMMAND_LIMIT 4096
/* Returned by transfer helpers when the requesting client has disconnected. */
#define CLIENT_GONE (-2)
/* A transfer peer silent for this long is considered gone. */
#define TRANSFER_STALL_SECONDS 30

static volatile sig_atomic_t stop_server;
static unsigned long long next_transfer_id;
static pid_t *handlers;
static size_t handler_count;
static size_t handler_capacity;

typedef struct pending_transfer {
    transfer_request_message request;
    struct pending_transfer *next;
} pending_transfer;

static void discard_pending_transfers(pending_transfer *pending)
{
    while (pending != NULL) {
        pending_transfer *next = pending->next;
        transfer_reply_message reply = {0};
        mqd_t sender_queue = mq_open(pending->request.reply_queue, O_WRONLY);

        if (sender_queue != (mqd_t)-1) {
            reply.type = TRANSFER_REJECT;
            mq_send(sender_queue, (const char *)&reply, sizeof(reply), 0);
            mq_close(sender_queue);
        }
        free(pending);
        pending = next;
    }
}

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_server = 1;
}

/* Only interrupts poll(); children are reaped in the main loop. */
static void wake_up(int signal_number)
{
    (void)signal_number;
}

static void track_handler(pid_t pid)
{
    if (handler_count == handler_capacity) {
        size_t capacity = handler_capacity == 0 ? 16 : handler_capacity * 2;
        pid_t *grown = realloc(handlers, capacity * sizeof(*handlers));
        if (grown == NULL) {
            fprintf(stderr, "warning: cannot track client handler %ld\n",
                    (long)pid);
            return;
        }
        handlers = grown;
        handler_capacity = capacity;
    }
    handlers[handler_count++] = pid;
}

static void reap_handlers(void)
{
    pid_t pid;

    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        size_t i;
        for (i = 0; i < handler_count; ++i) {
            if (handlers[i] == pid) {
                handlers[i] = handlers[--handler_count];
                break;
            }
        }
    }
}

static void stop_handlers(void)
{
    size_t i;

    /* Handlers run as other users after login: killing them needs root. */
    privilege_raise();
    for (i = 0; i < handler_count; ++i) {
        kill(handlers[i], SIGTERM);
    }
    privilege_lower();
    for (;;) {
        pid_t pid = waitpid(-1, NULL, 0);
        if (pid < 0 && errno == EINTR) {
            continue;
        }
        if (pid < 0) {
            break;
        }
    }
    free(handlers);
}

static int console_in_foreground(void)
{
    return !isatty(STDIN_FILENO) || tcgetpgrp(STDIN_FILENO) == getpgrp();
}

/* Returns 1 on "exit", -1 when stdin is closed, 0 otherwise. */
static int handle_console(void)
{
    char buffer[256];
    char *save_pointer = NULL;
    char *line;
    ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer) - 1);

    if (count < 0) {
        return errno == EINTR || errno == EIO || errno == EAGAIN ? 0 : -1;
    }
    if (count == 0) {
        return -1;
    }
    buffer[count] = '\0';
    for (line = strtok_r(buffer, "\r\n", &save_pointer); line != NULL;
         line = strtok_r(NULL, "\r\n", &save_pointer)) {
        char word[16];
        char extra;
        int words = sscanf(line, "%15s %c", word, &extra);

        if (words == 1 && strcmp(word, "exit") == 0) {
            return 1;
        }
        if (words > 0) {
            printf("Unknown server command '%s'. Type 'exit' to stop the server.\n",
                   line);
        }
    }
    fflush(stdout);
    return 0;
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

static mqd_t open_user_notification_queue(uid_t uid)
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

/* Queues the request; while the recipient has no queue yet, keep waiting. */
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
        ssize_t count;
        int status;

        do {
            count = read(source_fd, chunk.payload.chunk.data, TRANSFER_CHUNK);
        } while (count < 0 && errno == EINTR);
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

static int send_transfer_request(int client_fd, const fs_session *session,
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

/* Streams a file (used by both read and download) under a shared lock. */
static int send_file(int client_fd, const fs_session *session,
                     const char *path, off_t offset, const char *banner)
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
    if (send_text(client_fd, banner) < 0) {
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
        request->reply_queue[0] != '/' ||
        strncmp(request->reply_queue, TRANSFER_REPLY_PREFIX,
                strlen(TRANSFER_REPLY_PREFIX)) != 0 ||
        request->data_queue[0] != '/' ||
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

static int receive_transfer_request(mqd_t notification_queue, int client_fd,
                                    pending_transfer **pending)
{
    transfer_request_message request;
    ssize_t received = mq_receive(notification_queue, (char *)&request,
                                  sizeof(request), NULL);
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
             "\001NOTIFY Transfer request %s from %s for file %s. "
             "Use: accept <directory> %s  |  reject %s\n",
             request.request_id, request.sender, request.filename,
             request.request_id, request.request_id);
    return send_text(client_fd, notification);
}

static int finish_accepted_transfer(int client_fd, const fs_session *session,
                                    pending_transfer *entry,
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

    if (!valid_pending_request(&entry->request)) {
        return send_error(client_fd, "sender is no longer online");
    }
    sender_queue = mq_open(entry->request.reply_queue, O_WRONLY);
    if (sender_queue == (mqd_t)-1) {
        return send_error(client_fd, "cannot contact transfer sender");
    }
    data_queue = mq_open(entry->request.data_queue, O_RDONLY);
    if (data_queue == (mqd_t)-1) {
        mq_close(sender_queue);
        return send_error(client_fd, "cannot open transfer data channel");
    }
    if (fs_transfer_target(session, directory, entry->request.filename,
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
            if ((kill(entry->request.sender_pid, 0) < 0 && errno == ESRCH) ||
                ++idle_seconds >= TRANSFER_STALL_SECONDS) {
                failed = 1;
                snprintf(error, sizeof(error), "transfer sender disconnected");
                break;
            }
            continue;
        }
        idle_seconds = 0;
        if (received < 0) {
            failed = 1;
            snprintf(error, sizeof(error), "cannot receive transfer data");
            break;
        }
        if ((size_t)received != sizeof(reply)) {
            failed = 1;
            snprintf(error, sizeof(error), "invalid transfer data message");
            break;
        }
        if (reply.type == TRANSFER_END) {
            break;
        }
        if (reply.type == TRANSFER_ERROR) {
            failed = 1;
            snprintf(error, sizeof(error), "%.511s", reply.payload.error);
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
    if (mq_send(sender_queue, (const char *)&reply, sizeof(reply), 0) < 0) {
        mq_close(sender_queue);
        mq_close(data_queue);
        return send_error(client_fd, "cannot return transfer result to sender");
    }
    mq_close(sender_queue);
    mq_close(data_queue);
    if (failed) {
        return send_error(client_fd, error);
    }
    return send_text(client_fd, "OK: transfer accepted and file copied\n");
}

static int handle_transfer_decision(int client_fd, const fs_session *session,
                                    pending_transfer **pending,
                                    const char *command, char **arguments,
                                    size_t argument_count)
{
    pending_transfer **position = pending;
    const char *request_id;

    if (strcmp(command, "reject") == 0) {
        if (argument_count != 2) {
            return send_error(client_fd, "usage: reject <ID>");
        }
        request_id = arguments[1];
    } else {
        if (argument_count != 3) {
            return send_error(client_fd, "usage: accept <directory> <ID>");
        }
        request_id = arguments[2];
    }

    while (*position != NULL &&
           strcmp((*position)->request.request_id, request_id) != 0) {
        position = &(*position)->next;
    }
    if (*position == NULL) {
        return send_error(client_fd, "unknown or expired transfer request ID");
    }
    {
        pending_transfer *entry = *position;
        int result;

        if (strcmp(command, "accept") == 0) {
            result = finish_accepted_transfer(client_fd, session, entry,
                                              arguments[1]);
        } else {
            transfer_reply_message reply = {0};
            mqd_t sender_queue;

            reply.type = TRANSFER_REJECT;
            sender_queue = mq_open(entry->request.reply_queue, O_WRONLY);
            if (sender_queue == (mqd_t)-1 ||
                mq_send(sender_queue, (const char *)&reply,
                        sizeof(reply), 0) < 0) {
                if (sender_queue != (mqd_t)-1) {
                    mq_close(sender_queue);
                }
                return send_error(client_fd, "cannot notify transfer sender");
            }
            mq_close(sender_queue);
            result = send_text(client_fd, "OK: transfer request rejected\n");
        }
        *position = entry->next;
        free(entry);
        return result;
    }
}

static int execute_command(int client_fd, const char *root, fs_session *session,
                           int *logged_in, mqd_t *notification_queue,
                           pending_transfer **pending, char *command)
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
        fs_session candidate = {0};
        /* Client-side upload/download connections must not consume notifications. */
        int notify = argument_count == 2;
        int result;

        if (argument_count != 2 &&
            (argument_count != 3 || strcmp(arguments[2], "--no-notify") != 0)) {
            return send_error(client_fd, "usage: login <username>");
        }
        if (*logged_in) {
            return send_error(client_fd,
                              "already logged in; start a new client to change user");
        }
        if (privilege_raise() < 0) {
            return send_error(client_fd, "cannot acquire privileges for login");
        }
        result = fs_login(&candidate, root, arguments[1], error, sizeof(error));
        if (result == 0 &&
            privilege_become_user(candidate.username, candidate.uid,
                                  candidate.gid) < 0) {
            snprintf(error, sizeof(error),
                     "cannot switch to this user's account "
                     "(the server must run as root)");
            result = -1;
        }
        privilege_lower();
        if (result < 0) {
            return send_error(client_fd, error);
        }
        *session = candidate;
        if (notify) {
            *notification_queue = open_user_notification_queue(session->uid);
            if (*notification_queue == (mqd_t)-1) {
                return send_error(client_fd,
                                  "cannot open transfer notification queue");
            }
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
        if (privilege_raise() < 0) {
            return send_error(client_fd, "cannot acquire privileges for create_user");
        }
        if (fs_create_user(root, arguments[1], permissions,
                           error, sizeof(error)) < 0) {
            privilege_lower();
            return send_error(client_fd, error);
        }
        privilege_lower();
        return send_text(client_fd, "OK: user created\n");
    }
    if (strcmp(arguments[0], "exit") == 0) {
        return 1;
    }
    if (!*logged_in) {
        return send_error(client_fd, "log in first with: login <username>");
    }

    if (strcmp(arguments[0], "transfer_request") == 0) {
        if (argument_count != 3) {
            return send_error(client_fd,
                              "usage: transfer_request <file> <dest_user>");
        }
        return send_transfer_request(client_fd, session, arguments[1],
                                     arguments[2]);
    }
    if (strcmp(arguments[0], "accept") == 0 ||
        strcmp(arguments[0], "reject") == 0) {
        return handle_transfer_decision(client_fd, session, pending,
                                        arguments[0], arguments, argument_count);
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

    if (strcmp(arguments[0], "pwd") == 0) {
        char response[PATH_MAX + 2];

        if (argument_count != 1) {
            return send_error(client_fd, "usage: pwd");
        }
        snprintf(response, sizeof(response), "/%s\n", session->cwd);
        return send_text(client_fd, response);
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
        if (listing_length == 0) {
            free(listing);
            return send_text(client_fd, "(empty directory)\n");
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
        return send_file(client_fd, session, arguments[1], 0,
                         "OK: download begins\n");
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
        return send_file(client_fd, session, path, offset, "OK: read begins\n");
    }

    if (strcmp(arguments[0], "write") == 0) {
        off_t offset = -1;
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
    mqd_t notification_queue = (mqd_t)-1;
    pending_transfer *pending = NULL;

    for (;;) {
        unsigned char *request;
        size_t request_length;
        int result;
        struct pollfd descriptors[2];
        nfds_t descriptor_count = 1;
        int ready;

        descriptors[0].fd = client_fd;
        descriptors[0].events = POLLIN | POLLHUP | POLLERR;
        descriptors[0].revents = 0;
        if (notification_queue != (mqd_t)-1) {
            descriptors[1].fd = (int)notification_queue;
            descriptors[1].events = POLLIN;
            descriptors[1].revents = 0;
            descriptor_count = 2;
        }
        ready = poll(descriptors, descriptor_count, -1);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            break;
        }
        if (descriptor_count == 2 &&
            (descriptors[1].revents & POLLIN) != 0) {
            if (receive_transfer_request(notification_queue, client_fd,
                                         &pending) < 0) {
                break;
            }
        }
        if ((descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 &&
            (descriptors[0].revents & POLLIN) == 0) {
            break;
        }
        if ((descriptors[0].revents & POLLIN) == 0) {
            continue;
        }
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
                                 &notification_queue, &pending,
                                 (char *)request);
        free(request);
        if (result != 0) {
            break;
        }
    }
    if (notification_queue != (mqd_t)-1) {
        mq_close(notification_queue);
    }
    discard_pending_transfers(pending);
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
    int watch_console = 1;

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
    /* Handlers use paths relative to this cwd: after setuid the user may not
       be allowed to traverse the root's ancestors (e.g. a 0750 /home/x). */
    if (chdir(root) < 0) {
        perror("cannot enter server root");
        return EXIT_FAILURE;
    }

    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGINT, &action, NULL) < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    action.sa_handler = wake_up;
    action.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &action, NULL) < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    /* A background server must not be stopped when the shell owns the tty. */
    signal(SIGTTIN, SIG_IGN);

    listener = create_listener(ip, port);
    if (listener < 0) {
        return EXIT_FAILURE;
    }
    if (privilege_init() < 0) {
        perror("cannot drop root privileges");
        return EXIT_FAILURE;
    }
    printf("Server listening on %s:%s (root: %s). Type 'exit' to stop it.\n",
           ip, port, root);
    fflush(stdout);

    while (!stop_server) {
        struct pollfd descriptors[2] = {
            { .fd = listener, .events = POLLIN },
            { .fd = STDIN_FILENO, .events = POLLIN }
        };
        nfds_t descriptor_count =
            watch_console && console_in_foreground() ? 2 : 1;
        int client_fd;
        pid_t child;

        reap_handlers();
        /* The timeout bounds how long a zombie can survive a lost SIGCHLD race. */
        if (poll(descriptors, descriptor_count, 1000) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if (descriptor_count == 2 && descriptors[1].revents != 0) {
            int console = handle_console();
            if (console > 0) {
                break;
            }
            if (console < 0) {
                watch_console = 0;
            }
        }
        if ((descriptors[0].revents & POLLIN) == 0) {
            continue;
        }
        client_fd = accept(listener, NULL, NULL);
        if (client_fd < 0) {
            if (errno != EINTR && errno != ECONNABORTED) {
                perror("accept");
            }
            continue;
        }

        child = fork();
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
            sigaction(SIGINT, &action, NULL);
            sigaction(SIGTERM, &action, NULL);
            serve_client(client_fd, ".");
            close(client_fd);
            _exit(EXIT_SUCCESS);
        }
        track_handler(child);
        close(client_fd);
    }

    printf("Server shutting down.\n");
    close(listener);
    stop_handlers();
    return EXIT_SUCCESS;
}
