#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "session.h"
#include "filesystem.h"
#include "network.h"
#include "privilege.h"
#include "transfer.h"
#include "utils.h"

#include <errno.h>
#include <limits.h>
#include <mqueue.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COMMAND_LIMIT 4096

typedef struct {
    const char *root;
    fs_session fs;
    int logged_in;
    mqd_t inbox;
    pending_transfer *pending;
} client_state;

/* Handlers return 0 to go on, 1 to end the session, -1 on a broken connection. */
typedef int (*command_handler)(int client_fd, client_state *state,
                               char **arguments, size_t argument_count);

static int reply(int client_fd, int result, const char *error,
                 const char *success)
{
    return result < 0 ? send_error(client_fd, error)
                      : send_text(client_fd, success);
}

/* Parses "<command> [-offset=<num>] <path>"; offset is left as is without it. */
static const char *offset_path(char **arguments, size_t argument_count,
                               off_t *offset)
{
    if (argument_count == 2) {
        return arguments[1];
    }
    if (argument_count == 3 && parse_offset_option(arguments[1], offset) == 0) {
        return arguments[2];
    }
    return NULL;
}

static int command_login(int client_fd, client_state *state,
                         char **arguments, size_t argument_count)
{
    fs_session candidate = {0};
    /* The client's upload/download connections must not consume notifications. */
    int notify = argument_count == 2;
    char error[512];
    int result;

    if (argument_count != 2 &&
        (argument_count != 3 || strcmp(arguments[2], "--no-notify") != 0)) {
        return send_error(client_fd, "usage: login <username>");
    }
    if (state->logged_in) {
        return send_error(client_fd,
                          "already logged in; start a new client to change user");
    }
    if (privilege_raise() < 0) {
        return send_error(client_fd, "cannot acquire privileges for login");
    }
    result = fs_login(&candidate, state->root, arguments[1], error, sizeof(error));
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
    state->fs = candidate;
    state->logged_in = 1;
    if (notify) {
        state->inbox = transfer_open_inbox(state->fs.uid);
        if (state->inbox == (mqd_t)-1) {
            return send_text(client_fd, "OK: logged in, but transfer "
                                        "notifications are unavailable\n");
        }
    }
    return send_text(client_fd, "OK: logged in\n");
}

static int command_create_user(int client_fd, client_state *state,
                               char **arguments, size_t argument_count)
{
    char error[512];
    mode_t permissions;
    int result;

    if (argument_count != 3 ||
        parse_permissions(arguments[2], &permissions) < 0) {
        return send_error(client_fd,
                          "usage: create_user <username> <permissions in octal>");
    }
    if (privilege_raise() < 0) {
        return send_error(client_fd, "cannot acquire privileges for create_user");
    }
    result = fs_create_user(state->root, arguments[1], permissions,
                            error, sizeof(error));
    privilege_lower();
    return reply(client_fd, result, error, "OK: user created\n");
}

static int command_exit(int client_fd, client_state *state,
                        char **arguments, size_t argument_count)
{
    (void)client_fd;
    (void)state;
    (void)arguments;
    (void)argument_count;
    return 1;
}

static int command_create(int client_fd, client_state *state,
                          char **arguments, size_t argument_count)
{
    char error[512];
    mode_t permissions;
    int directory = argument_count == 4 &&
                    (strcmp(arguments[1], "-d") == 0 ||
                     strcmp(arguments[3], "-d") == 0);
    const char *path;
    const char *mode;

    if (directory && strcmp(arguments[1], "-d") == 0) {
        path = arguments[2];
        mode = arguments[3];
    } else if (directory || argument_count == 3) {
        path = arguments[1];
        mode = arguments[2];
    } else {
        return send_error(client_fd, "usage: create [-d] <path> <permissions>");
    }
    if (parse_permissions(mode, &permissions) < 0) {
        return send_error(client_fd, "permissions must be octal (0000-0777)");
    }
    return reply(client_fd,
                 fs_create(&state->fs, path, permissions, directory,
                           error, sizeof(error)),
                 error, "OK: created\n");
}

static int command_chmod(int client_fd, client_state *state,
                         char **arguments, size_t argument_count)
{
    char error[512];
    mode_t permissions;

    if (argument_count != 3 ||
        parse_permissions(arguments[2], &permissions) < 0) {
        return send_error(client_fd, "usage: chmod <path> <permissions>");
    }
    return reply(client_fd,
                 fs_chmod(&state->fs, arguments[1], permissions,
                          error, sizeof(error)),
                 error, "OK: permissions changed\n");
}

static int command_move(int client_fd, client_state *state,
                        char **arguments, size_t argument_count)
{
    char error[512];

    if (argument_count != 3) {
        return send_error(client_fd, "usage: move <path1> <path2>");
    }
    return reply(client_fd,
                 fs_move(&state->fs, arguments[1], arguments[2],
                         error, sizeof(error)),
                 error, "OK: moved\n");
}

static int command_pwd(int client_fd, client_state *state,
                       char **arguments, size_t argument_count)
{
    char response[PATH_MAX + 2];

    (void)arguments;
    if (argument_count != 1) {
        return send_error(client_fd, "usage: pwd");
    }
    snprintf(response, sizeof(response), "/%s\n", state->fs.cwd);
    return send_text(client_fd, response);
}

static int command_cd(int client_fd, client_state *state,
                      char **arguments, size_t argument_count)
{
    char error[512];

    if (argument_count != 2) {
        return send_error(client_fd, "usage: cd <path>");
    }
    return reply(client_fd,
                 fs_change_directory(&state->fs, arguments[1],
                                     error, sizeof(error)),
                 error, "OK: directory changed\n");
}

static int command_list(int client_fd, client_state *state,
                        char **arguments, size_t argument_count)
{
    char error[512];
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
        result = fs_list_other(&state->fs, arguments[1] + 1,
                               other_path == NULL ? "." : other_path,
                               &listing, &listing_length,
                               error, sizeof(error));
    } else {
        result = fs_list(&state->fs, argument_count == 2 ? arguments[1] : ".",
                         &listing, &listing_length, error, sizeof(error));
    }
    if (result < 0) {
        return send_error(client_fd, error);
    }
    result = listing_length == 0
                 ? send_text(client_fd, "(empty directory)\n")
                 : send_frame(client_fd, listing, listing_length);
    free(listing);
    return result;
}

/* Upload data arrives as frames terminated by an empty one. */
static int command_upload(int client_fd, client_state *state,
                          char **arguments, size_t argument_count)
{
    char error[512];
    int file_fd = -1;
    int opened;

    if (argument_count != 2) {
        return send_error(client_fd, "usage: upload <server path>");
    }
    opened = fs_upload_begin(&state->fs, arguments[1], &file_fd,
                             error, sizeof(error)) == 0;
    for (;;) {
        unsigned char *chunk;
        size_t chunk_length;
        int failed;

        if (receive_frame(client_fd, &chunk, &chunk_length) < 0) {
            if (opened) {
                fs_upload_finish(file_fd, 0, error, sizeof(error));
            }
            return -1;
        }
        /* After an error the data is still drained to keep the protocol in sync. */
        failed = opened && chunk_length > 0 &&
                 write_all(file_fd, chunk, chunk_length) < 0;
        free(chunk);
        if (failed) {
            fs_upload_finish(file_fd, 0, error, sizeof(error));
            return -1;
        }
        if (chunk_length == 0) {
            break;
        }
    }
    if (!opened) {
        return send_error(client_fd, error);
    }
    return reply(client_fd, fs_upload_finish(file_fd, 1, error, sizeof(error)),
                 error, "OK: upload complete\n");
}

/* Streams a file (read and download) under a shared lock. */
static int send_file(int client_fd, const fs_session *session,
                     const char *path, off_t offset, const char *banner)
{
    unsigned char buffer[NETWORK_CHUNK];
    char error[512];
    int file_fd;
    int result = 0;

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
        ssize_t count = read_retry(file_fd, buffer, sizeof(buffer));

        /* An empty frame (count == 0) marks the end of the file. */
        if (count < 0 || send_frame(client_fd, buffer, (size_t)count) < 0) {
            result = -1;
            break;
        }
        if (count == 0) {
            break;
        }
    }
    close(file_fd);
    return result;
}

static int command_download(int client_fd, client_state *state,
                            char **arguments, size_t argument_count)
{
    if (argument_count != 2) {
        return send_error(client_fd, "usage: download <server path>");
    }
    return send_file(client_fd, &state->fs, arguments[1], 0,
                     "OK: download begins\n");
}

static int command_read(int client_fd, client_state *state,
                        char **arguments, size_t argument_count)
{
    off_t offset = 0;
    const char *path = offset_path(arguments, argument_count, &offset);

    if (path == NULL) {
        return send_error(client_fd, "usage: read [-offset=<num>] <path>");
    }
    return send_file(client_fd, &state->fs, path, offset, "OK: read begins\n");
}

/* The content follows the command in a separate frame. */
static int command_write(int client_fd, client_state *state,
                         char **arguments, size_t argument_count)
{
    off_t offset = -1;
    const char *path = offset_path(arguments, argument_count, &offset);
    unsigned char *contents;
    size_t contents_length;
    char error[512];
    int result;

    if (path == NULL) {
        return send_error(client_fd, "usage: write [-offset=<num>] <path>");
    }
    if (receive_frame(client_fd, &contents, &contents_length) < 0) {
        return -1;
    }
    result = fs_write(&state->fs, path, offset, contents, contents_length,
                      error, sizeof(error));
    free(contents);
    return reply(client_fd, result, error, "OK: file written\n");
}

static int command_delete(int client_fd, client_state *state,
                          char **arguments, size_t argument_count)
{
    char error[512];

    if (argument_count != 2) {
        return send_error(client_fd, "usage: delete <path>");
    }
    return reply(client_fd,
                 fs_delete(&state->fs, arguments[1], error, sizeof(error)),
                 error, "OK: deleted\n");
}

static int command_transfer_request(int client_fd, client_state *state,
                                    char **arguments, size_t argument_count)
{
    if (argument_count != 3) {
        return send_error(client_fd, "usage: transfer_request <file> <dest_user>");
    }
    return transfer_request(client_fd, &state->fs, arguments[1], arguments[2]);
}

static int command_transfer_decision(int client_fd, client_state *state,
                                     char **arguments, size_t argument_count)
{
    return transfer_decide(client_fd, &state->fs, &state->pending,
                           arguments, argument_count);
}

static const struct {
    const char *name;
    command_handler handler;
    int needs_login;
} commands[] = {
    { "login", command_login, 0 },
    { "create_user", command_create_user, 0 },
    { "exit", command_exit, 0 },
    { "create", command_create, 1 },
    { "chmod", command_chmod, 1 },
    { "move", command_move, 1 },
    { "pwd", command_pwd, 1 },
    { "cd", command_cd, 1 },
    { "list", command_list, 1 },
    { "upload", command_upload, 1 },
    { "download", command_download, 1 },
    { "read", command_read, 1 },
    { "write", command_write, 1 },
    { "delete", command_delete, 1 },
    { "transfer_request", command_transfer_request, 1 },
    { "accept", command_transfer_decision, 1 },
    { "reject", command_transfer_decision, 1 },
};

static int execute_command(int client_fd, client_state *state, char *line)
{
    char *arguments[MAX_ARGUMENTS];
    size_t argument_count = split_arguments(line, arguments, MAX_ARGUMENTS);
    size_t i;

    if (argument_count == 0) {
        return send_error(client_fd, "empty command");
    }
    if (argument_count == MAX_ARGUMENTS) {
        return send_error(client_fd, "too many command arguments");
    }
    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (strcmp(arguments[0], commands[i].name) != 0) {
            continue;
        }
        if (commands[i].needs_login && !state->logged_in) {
            return send_error(client_fd, "log in first with: login <username>");
        }
        return commands[i].handler(client_fd, state, arguments, argument_count);
    }
    return send_error(client_fd, "unknown command");
}

void session_serve(int client_fd, const char *root)
{
    client_state state = { .root = root, .inbox = (mqd_t)-1 };

    for (;;) {
        unsigned char *request;
        size_t request_length;
        struct pollfd descriptors[2] = {
            { .fd = client_fd, .events = POLLIN },
            { .fd = (int)state.inbox, .events = POLLIN }
        };
        nfds_t descriptor_count = state.inbox != (mqd_t)-1 ? 2 : 1;
        int result;

        /* Waits for a command or, once logged in, for a transfer request. */
        if (poll(descriptors, descriptor_count, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (descriptor_count == 2 && (descriptors[1].revents & POLLIN) != 0 &&
            transfer_notify(state.inbox, client_fd, &state.pending) < 0) {
            break;
        }
        if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
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
        result = execute_command(client_fd, &state, (char *)request);
        free(request);
        if (result != 0) {
            break;
        }
    }
    if (state.inbox != (mqd_t)-1) {
        mq_close(state.inbox);
    }
    transfer_discard_pending(state.pending);
}
