#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct {
    char root[PATH_MAX];
    char home[PATH_MAX];
    char cwd[PATH_MAX];
    char username[128];
    uid_t uid;
    gid_t gid;
} fs_session;

int fs_initialize_root(const char *root_path, char *error, size_t error_size);
int fs_create_user(const char *root, const char *username, mode_t permissions,
                   char *error, size_t error_size);
int fs_login(fs_session *session, const char *root, const char *username,
             char *error, size_t error_size);
int fs_create(const fs_session *session, const char *path, mode_t permissions,
              int directory, char *error, size_t error_size);
int fs_chmod(const fs_session *session, const char *path, mode_t permissions,
             char *error, size_t error_size);
int fs_move(const fs_session *session, const char *source, const char *destination,
            char *error, size_t error_size);
int fs_list(const fs_session *session, const char *path, char **output,
            size_t *output_length, char *error, size_t error_size);
int fs_list_other(const fs_session *session, const char *username,
                  const char *path, char **output, size_t *output_length,
                  char *error, size_t error_size);
/* A negative offset replaces the whole file content. */
int fs_write(const fs_session *session, const char *path, off_t offset,
             const unsigned char *data, size_t data_length,
             char *error, size_t error_size);
int fs_delete(const fs_session *session, const char *path,
              char *error, size_t error_size);
int fs_change_directory(fs_session *session, const char *path,
                        char *error, size_t error_size);
int fs_upload_begin(const fs_session *session, const char *path, int *file_fd,
                    char *error, size_t error_size);
int fs_upload_finish(int file_fd, int commit,
                     char *error, size_t error_size);
int fs_download_open(const fs_session *session, const char *path, int *file_fd,
                     char *error, size_t error_size);
int fs_transfer_source(const fs_session *session, const char *path, int *file_fd,
                       char *filename, size_t filename_size,
                       char *error, size_t error_size);
int fs_transfer_target(const fs_session *session, const char *directory,
                       const char *filename, int *file_fd, char *target_path,
                       size_t target_path_size, char *error, size_t error_size);
int fs_transfer_finish(int file_fd, const char *target_path, int commit,
                       char *error, size_t error_size);

#endif
