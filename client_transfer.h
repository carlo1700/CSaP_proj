#ifndef CLIENT_TRANSFER_H
#define CLIENT_TRANSFER_H

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

#define MAX_BACKGROUND_JOBS 64

typedef struct {
    char operation[16];
    char first_path[PATH_MAX];
    char second_path[PATH_MAX];
    /* Server-side cwd, so that the transfer connection starts from it. */
    char cwd[PATH_MAX];
    int background;
} transfer_command;

typedef struct {
    pid_t pids[MAX_BACKGROUND_JOBS];
    size_t count;
} background_jobs;

/* Receives an answer, printing the notifications that arrive before it. */
int receive_response(int socket_fd, unsigned char **data, size_t *length);
/* Returns 1 for upload/download, 0 for other commands, -1 if malformed. */
int parse_transfer_command(const char *line, transfer_command *transfer);
/* Prints a remote file on stdout. Returns 1 if refused, -1 if disconnected. */
int read_remote_file(int socket_fd, const char *command);
int query_cwd(int socket_fd, char *cwd, size_t cwd_size);
/* Runs an upload/download on a new connection logged in as username. */
int perform_transfer(const char *ip, const char *port, const char *username,
                     const transfer_command *transfer);
int start_background_transfer(background_jobs *jobs, const char *ip,
                              const char *port, const char *username,
                              int control_socket,
                              const transfer_command *transfer);
void reap_background_jobs(background_jobs *jobs);
void wait_background_jobs(background_jobs *jobs);

#endif
