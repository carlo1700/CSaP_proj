#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "filesystem.h"
#include "network.h"
#include "utils.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Error helpers: they fill the caller's error buffer and return -1. */
static int fail(char *error, size_t error_size, const char *message)
{
    snprintf(error, error_size, "%s: %s", message, strerror(errno));
    return -1;
}

static int fail_with(char *error, size_t error_size, int code,
                     const char *message)
{
    errno = code;
    return fail(error, error_size, message);
}

static int fail_plain(char *error, size_t error_size, const char *message)
{
    snprintf(error, error_size, "%s", message);
    return -1;
}

static int fail_close(int descriptor, char *error, size_t error_size,
                      const char *message)
{
    fail(error, error_size, message);
    close(descriptor);
    return -1;
}

/*
 * Opens and locks an existing path. Returns -1 if it cannot be opened: then
 * nobody without the same rights can be using it, so callers go on unlocked.
 */
static int open_locked(const char *path, int operation)
{
    int descriptor = open(path, O_RDONLY | O_NOFOLLOW);

    if (descriptor < 0 && errno == EACCES) {
        descriptor = open(path, O_WRONLY | O_NOFOLLOW | O_NONBLOCK);
    }
    if (descriptor < 0) {
        return -1;
    }
    while (flock(descriptor, operation) < 0) {
        if (errno != EINTR) {
            close(descriptor);
            return -1;
        }
    }
    return descriptor;
}

static void close_locked(int descriptor)
{
    if (descriptor >= 0) {
        close(descriptor);
    }
}

static int valid_username(const char *username)
{
    size_t i;

    if (username == NULL || username[0] == '\0' || username[0] == '.' ||
        strlen(username) >= sizeof(((fs_session *)0)->username)) {
        return 0;
    }
    for (i = 0; username[i] != '\0'; ++i) {
        unsigned char character = (unsigned char)username[i];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '_' || character == '-')) {
            return 0;
        }
    }
    return 1;
}

/* Puts <root>/users/<username> in path; 0 if it is an existing directory. */
static int find_home(const char *root, const char *username, char *path,
                     size_t path_size, struct stat *status)
{
    if (snprintf(path, path_size, "%s/users/%s", root, username) >=
        (int)path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (lstat(path, status) < 0) {
        return -1;
    }
    if (!S_ISDIR(status->st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

int fs_initialize_root(const char *root_path, char *error, size_t error_size)
{
    char canonical_root[PATH_MAX];
    char users_path[PATH_MAX];
    struct group *shared_group = geteuid() == 0 ? getgrnam("users") : NULL;
    struct stat status;
    int root_created = mkdir(root_path, 0750) == 0;

    if (!root_created && errno != EEXIST) {
        return fail(error, error_size, "cannot create root directory");
    }
    if (realpath(root_path, canonical_root) == NULL) {
        return fail(error, error_size, "cannot resolve root directory");
    }
    if (strcmp(canonical_root, "/") == 0) {
        return fail_with(error, error_size, EINVAL,
                         "server root cannot be the filesystem root");
    }
    if (geteuid() == 0 && shared_group == NULL) {
        return fail_plain(error, error_size,
                          "required shared system group 'users' is missing");
    }
    if (root_created && shared_group != NULL &&
        (chown(canonical_root, 0, shared_group->gr_gid) < 0 ||
         chmod(canonical_root, 0711) < 0)) {
        return fail(error, error_size, "cannot configure root directory");
    }
    if (snprintf(users_path, sizeof(users_path), "%s/users", canonical_root) >=
        (int)sizeof(users_path)) {
        return fail_with(error, error_size, ENAMETOOLONG,
                         "root directory path is too long");
    }
    if (mkdir(users_path, 0750) < 0 && errno != EEXIST) {
        return fail(error, error_size, "cannot create users directory");
    }
    if (lstat(users_path, &status) < 0 || !S_ISDIR(status.st_mode)) {
        return fail_with(error, error_size, ENOTDIR,
                         "users path must be a directory");
    }
    /* Group "users" may list the homes; "other" gets nothing. */
    if (shared_group != NULL &&
        (chown(users_path, 0, shared_group->gr_gid) < 0 ||
         chmod(users_path, 0750) < 0)) {
        return fail(error, error_size, "cannot configure users directory");
    }
    return 0;
}

int fs_create_user(const char *root, const char *username, mode_t permissions,
                   char *error, size_t error_size)
{
    char user_path[PATH_MAX];
    struct stat status;
    struct group *shared_group;
    struct passwd *user;
    gid_t users_gid;
    pid_t child;
    int child_status;

    if (!valid_username(username)) {
        return fail_plain(error, error_size, "invalid username");
    }
    if (geteuid() != 0) {
        return fail_plain(error, error_size,
                          "create_user requires the server to run with root privileges");
    }
    shared_group = getgrnam("users");
    if (shared_group == NULL) {
        return fail_plain(error, error_size,
                          "required shared system group 'users' is missing");
    }
    users_gid = shared_group->gr_gid;
    if (getpwnam(username) != NULL) {
        return fail_plain(error, error_size,
                          "a system user with that username already exists");
    }
    if (find_home(root, username, user_path, sizeof(user_path), &status) == 0) {
        return fail_plain(error, error_size,
                          "a home directory with that username already exists");
    }
    if (errno != ENOENT) {
        return fail(error, error_size, "cannot check user home path");
    }

    child = fork();
    if (child < 0) {
        return fail(error, error_size, "cannot start adduser");
    }
    if (child == 0) {
        execlp("adduser", "adduser", "--disabled-password", "--gecos", "",
               "--ingroup", "users", username, (char *)NULL);
        _exit(127);
    }
    while (waitpid(child, &child_status, 0) < 0) {
        if (errno != EINTR) {
            return fail(error, error_size, "cannot wait for adduser");
        }
    }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        return fail_plain(error, error_size, "adduser failed to create system account");
    }
    user = getpwnam(username);
    if (user == NULL || user->pw_gid != users_gid) {
        return fail_plain(error, error_size,
                          "created account is not in the shared users group");
    }
    /* chmod after mkdir, because mkdir applies the umask. */
    if (mkdir(user_path, permissions) < 0 ||
        chown(user_path, user->pw_uid, users_gid) < 0 ||
        chmod(user_path, permissions) < 0) {
        return fail(error, error_size, "cannot create user home");
    }
    return 0;
}

int fs_login(fs_session *session, const char *root, const char *username,
             char *error, size_t error_size)
{
    char user_path[PATH_MAX];
    struct stat status;
    struct passwd *user;

    if (!valid_username(username)) {
        return fail_plain(error, error_size, "invalid username");
    }
    if (find_home(root, username, user_path, sizeof(user_path), &status) < 0) {
        return fail_with(error, error_size, ENOENT, "user does not exist");
    }
    user = getpwnam(username);
    if (user == NULL) {
        return fail_plain(error, error_size,
                          "user has no corresponding system account");
    }
    if (status.st_uid != user->pw_uid) {
        return fail_with(error, error_size, EACCES,
                         "user home has the wrong system owner");
    }

    snprintf(session->root, sizeof(session->root), "%s", root);
    /* user_path has no symlinks or "..": valid_username and lstat checked it. */
    snprintf(session->home, sizeof(session->home), "%s", user_path);
    snprintf(session->username, sizeof(session->username), "%s", username);
    session->uid = user->pw_uid;
    session->gid = user->pw_gid;
    session->cwd[0] = '\0';
    return 0;
}

int fs_resolve_path(const fs_session *session, const char *path,
                    int allow_missing_leaf, char *resolved, size_t resolved_size,
                    char *error, size_t error_size)
{
    char combined[PATH_MAX];
    size_t home_length = strlen(session->home);
    size_t output_length = home_length;
    char *cursor = combined;
    struct stat status;

    /* "/" is the home and relative paths start from the cwd. */
    if (snprintf(combined, sizeof(combined), "%s/%s",
                 path[0] == '/' ? "" : session->cwd, path) >=
            (int)sizeof(combined) ||
        home_length >= resolved_size) {
        return fail_with(error, error_size, ENAMETOOLONG, "path is too long");
    }
    memcpy(resolved, session->home, home_length + 1);

    /* Walks the path one component at a time, checking each with lstat. */
    for (;;) {
        char *component;
        size_t length;
        int is_last;

        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            return 0;
        }
        component = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        length = (size_t)(cursor - component);
        is_last = cursor[strspn(cursor, "/")] == '\0';

        if (length == 1 && component[0] == '.') {
            continue;
        }
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            /* ".." removes the last component, but never goes above the home. */
            while (output_length > home_length &&
                   resolved[--output_length] != '/') {
            }
            resolved[output_length] = '\0';
            continue;
        }
        if (output_length + length + 2 > resolved_size) {
            return fail_with(error, error_size, ENAMETOOLONG, "path is too long");
        }
        resolved[output_length++] = '/';
        memcpy(resolved + output_length, component, length);
        output_length += length;
        resolved[output_length] = '\0';

        if (lstat(resolved, &status) < 0) {
            if (errno == ENOENT && allow_missing_leaf && is_last) {
                return 0;
            }
            return fail(error, error_size, "cannot access path");
        }
        if (S_ISLNK(status.st_mode)) {
            return fail_with(error, error_size, EACCES,
                             "symbolic links are not allowed");
        }
        if (!is_last && !S_ISDIR(status.st_mode)) {
            return fail_with(error, error_size, ENOTDIR,
                             "path component is not a directory");
        }
    }
}

int fs_create(const fs_session *session, const char *path, mode_t permissions,
              int directory, char *error, size_t error_size)
{
    char resolved[PATH_MAX];
    int descriptor;

    if (fs_resolve_path(session, path, 1, resolved, sizeof(resolved),
                        error, error_size) < 0) {
        return -1;
    }
    /* chmod after mkdir/open, because they apply the umask. */
    if (directory) {
        if (mkdir(resolved, permissions) < 0 || chmod(resolved, permissions) < 0) {
            return fail(error, error_size, "cannot create directory");
        }
        return 0;
    }
    descriptor = open(resolved, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                      permissions);
    if (descriptor < 0) {
        return fail(error, error_size, "cannot create file");
    }
    if (fchmod(descriptor, permissions) < 0) {
        return fail_close(descriptor, error, error_size,
                          "cannot set file permissions");
    }
    close(descriptor);
    return 0;
}

int fs_chmod(const fs_session *session, const char *path, mode_t permissions,
             char *error, size_t error_size)
{
    char resolved[PATH_MAX];
    int descriptor;
    int result;

    if (fs_resolve_path(session, path, 0, resolved, sizeof(resolved),
                        error, error_size) < 0) {
        return -1;
    }
    descriptor = open_locked(resolved, LOCK_EX);
    result = chmod(resolved, permissions);
    if (result < 0) {
        fail(error, error_size, "cannot change permissions");
    }
    close_locked(descriptor);
    return result;
}

int fs_move(const fs_session *session, const char *source, const char *destination,
            char *error, size_t error_size)
{
    char source_path[PATH_MAX];
    char destination_path[PATH_MAX];
    struct stat status;
    int source_fd;
    int destination_fd = -1;
    int result;

    if (fs_resolve_path(session, source, 0, source_path, sizeof(source_path),
                        error, error_size) < 0 ||
        fs_resolve_path(session, destination, 1, destination_path,
                        sizeof(destination_path), error, error_size) < 0) {
        return -1;
    }
    if (strcmp(source_path, session->home) == 0) {
        return fail_with(error, error_size, EBUSY, "cannot move user home");
    }
    /* Like mv: moving onto an existing directory places the source inside it. */
    if (stat(destination_path, &status) == 0 && S_ISDIR(status.st_mode)) {
        size_t length = strlen(destination_path);
        if (snprintf(destination_path + length, sizeof(destination_path) - length,
                     "/%s", strrchr(source_path, '/') + 1) >=
            (int)(sizeof(destination_path) - length)) {
            return fail_with(error, error_size, ENAMETOOLONG,
                             "destination path is too long");
        }
    }
    /* Fixed lock order, so that "move a b" and "move b a" cannot deadlock. */
    if (strcmp(source_path, destination_path) < 0) {
        source_fd = open_locked(source_path, LOCK_EX);
        destination_fd = open_locked(destination_path, LOCK_EX);
    } else {
        destination_fd = strcmp(source_path, destination_path) != 0
                             ? open_locked(destination_path, LOCK_EX) : -1;
        source_fd = open_locked(source_path, LOCK_EX);
    }
    result = rename(source_path, destination_path);
    if (result < 0) {
        fail(error, error_size, "cannot move path");
    }
    close_locked(destination_fd);
    close_locked(source_fd);
    return result;
}

/* Builds "<permissions>\t<name>\t<size> bytes" lines in a memory stream. */
static int list_directory(const fs_session *session, const char *path,
                          char **output, size_t *output_length,
                          char *error, size_t error_size)
{
    char resolved[PATH_MAX];
    struct dirent *entry;
    DIR *directory;
    FILE *listing;
    int result = 0;

    if (fs_resolve_path(session, path, 0, resolved, sizeof(resolved),
                        error, error_size) < 0) {
        return -1;
    }
    directory = opendir(resolved);
    if (directory == NULL) {
        return fail(error, error_size, "cannot list directory");
    }
    listing = open_memstream(output, output_length);
    if (listing == NULL) {
        fail(error, error_size, "cannot list directory");
        closedir(directory);
        return -1;
    }

    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char entry_path[PATH_MAX];
        char permissions[10];
        struct stat status;
        int too_long;
        int bit;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        too_long = snprintf(entry_path, sizeof(entry_path), "%s/%s", resolved,
                            entry->d_name) >= (int)sizeof(entry_path);
        if (too_long || lstat(entry_path, &status) < 0) {
            result = fail_with(error, error_size, too_long ? ENAMETOOLONG : errno,
                               "cannot read directory entry");
            break;
        }
        for (bit = 0; bit < 9; ++bit) {
            permissions[bit] = status.st_mode & (0400 >> bit) ? "rwxrwxrwx"[bit] : '-';
        }
        permissions[9] = '\0';
        fprintf(listing, "%s\t%s\t%lld bytes\n", permissions, entry->d_name,
                (long long)status.st_size);
        errno = 0;
    }
    if (result == 0 && errno != 0) {
        result = fail(error, error_size, "cannot read directory");
    }
    closedir(directory);
    if (fclose(listing) != 0 && result == 0) {
        result = fail(error, error_size, "cannot build directory listing");
    }
    if (result == 0 && *output_length > NETWORK_MAX_FRAME) {
        result = fail_with(error, error_size, EOVERFLOW,
                           "directory listing is too large");
    }
    if (result < 0) {
        free(*output);
    }
    return result;
}

/*
 * list may leave the home: paths are resolved inside <root>/users, starting
 * from the user's cwd, while "/" still denotes the user's home.
 */
int fs_list(const fs_session *session, const char *path, char **output,
            size_t *output_length, char *error, size_t error_size)
{
    fs_session view = *session;
    char absolute[PATH_MAX];

    if (snprintf(view.home, sizeof(view.home), "%s/users", session->root) >=
            (int)sizeof(view.home) ||
        snprintf(view.cwd, sizeof(view.cwd), "%s%s%s", session->username,
                 session->cwd[0] != '\0' ? "/" : "", session->cwd) >=
            (int)sizeof(view.cwd) ||
        (path[0] == '/' &&
         snprintf(absolute, sizeof(absolute), "/%s%s", session->username,
                  path) >= (int)sizeof(absolute))) {
        return fail_with(error, error_size, ENAMETOOLONG, "path is too long");
    }
    if (path[0] == '/') {
        path = absolute;
    }
    return list_directory(&view, path, output, output_length,
                          error, error_size);
}

int fs_list_other(const fs_session *session, const char *username,
                  const char *path, char **output, size_t *output_length,
                  char *error, size_t error_size)
{
    fs_session other = *session;
    struct stat status;

    if (!valid_username(username)) {
        return fail_plain(error, error_size, "invalid username in list path");
    }
    if (find_home(session->root, username, other.home, sizeof(other.home),
                  &status) < 0) {
        return fail_with(error, error_size, ENOENT, "user home does not exist");
    }
    other.cwd[0] = '\0';
    return list_directory(&other, path, output, output_length,
                          error, error_size);
}

int fs_write(const fs_session *session, const char *path, off_t offset,
             const unsigned char *data, size_t data_length,
             char *error, size_t error_size)
{
    char resolved[PATH_MAX];
    int descriptor;
    int replace = offset < 0;

    if (fs_resolve_path(session, path, 1, resolved, sizeof(resolved),
                        error, error_size) < 0) {
        return -1;
    }
    descriptor = open(resolved, O_WRONLY | O_CREAT | O_NOFOLLOW, 0700);
    if (descriptor < 0) {
        return fail(error, error_size, "cannot open file for writing");
    }
    if (flock(descriptor, LOCK_EX) < 0 ||
        (replace && ftruncate(descriptor, 0) < 0) ||
        lseek(descriptor, replace ? 0 : offset, SEEK_SET) < 0) {
        return fail_close(descriptor, error, error_size,
                          "cannot lock or seek file for writing");
    }
    if (write_all(descriptor, data, data_length) < 0) {
        return fail_close(descriptor, error, error_size, "cannot write file");
    }
    close(descriptor);
    return 0;
}

int fs_delete(const fs_session *session, const char *path,
              char *error, size_t error_size)
{
    char resolved[PATH_MAX];
    struct stat status;
    int descriptor;
    int result;

    if (fs_resolve_path(session, path, 0, resolved, sizeof(resolved),
                        error, error_size) < 0) {
        return -1;
    }
    if (strcmp(resolved, session->home) == 0) {
        return fail_with(error, error_size, EBUSY, "cannot delete user home");
    }
    if (lstat(resolved, &status) < 0) {
        return fail(error, error_size, "cannot access path to delete");
    }
    if (S_ISDIR(status.st_mode)) {
        return rmdir(resolved) < 0
                   ? fail(error, error_size, "cannot delete directory") : 0;
    }
    /* Deleting is a write: wait for readers and writers to finish. */
    descriptor = open_locked(resolved, LOCK_EX);
    result = unlink(resolved);
    if (result < 0) {
        fail(error, error_size, "cannot delete file");
    }
    close_locked(descriptor);
    return result;
}

int fs_change_directory(fs_session *session, const char *path,
                        char *error, size_t error_size)
{
    char resolved[PATH_MAX];
    struct stat status;
    size_t home_length = strlen(session->home);

    if (fs_resolve_path(session, path, 0, resolved, sizeof(resolved),
                        error, error_size) < 0) {
        return -1;
    }
    if (stat(resolved, &status) < 0 || !S_ISDIR(status.st_mode)) {
        return fail_with(error, error_size, ENOTDIR, "not a directory");
    }
    snprintf(session->cwd, sizeof(session->cwd), "%s",
             resolved[home_length] == '\0' ? "" : resolved + home_length + 1);
    return 0;
}

int fs_upload_begin(const fs_session *session, const char *path, int *file_fd,
                    char *error, size_t error_size)
{
    char resolved[PATH_MAX];
    struct stat status;
    int descriptor;

    if (fs_resolve_path(session, path, 1, resolved, sizeof(resolved),
                        error, error_size) < 0) {
        return -1;
    }
    descriptor = open(resolved, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK,
                      0600);
    if (descriptor < 0) {
        return fail(error, error_size, "cannot open upload destination");
    }
    if (fstat(descriptor, &status) < 0 || !S_ISREG(status.st_mode)) {
        errno = EINVAL;
        return fail_close(descriptor, error, error_size,
                          "upload destination must be a regular file");
    }
    if (flock(descriptor, LOCK_EX) < 0 || ftruncate(descriptor, 0) < 0) {
        return fail_close(descriptor, error, error_size,
                          "cannot lock or prepare upload destination");
    }
    *file_fd = descriptor;
    return 0;
}

int fs_upload_finish(int file_fd, int commit,
                     char *error, size_t error_size)
{
    /* An interrupted upload leaves an empty file rather than a partial one. */
    int result = commit ? 0 : ftruncate(file_fd, 0);

    if (close(file_fd) < 0 || result < 0) {
        return fail(error, error_size, "cannot finish upload");
    }
    return 0;
}

int fs_download_open(const fs_session *session, const char *path, int *file_fd,
                     char *error, size_t error_size)
{
    char resolved[PATH_MAX];
    struct stat status;
    int descriptor;

    if (fs_resolve_path(session, path, 0, resolved, sizeof(resolved),
                        error, error_size) < 0) {
        return -1;
    }
    descriptor = open(resolved, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) {
        return fail(error, error_size, "cannot open file");
    }
    if (fstat(descriptor, &status) < 0 || !S_ISREG(status.st_mode)) {
        errno = EINVAL;
        return fail_close(descriptor, error, error_size,
                          "path is not a regular file");
    }
    if (flock(descriptor, LOCK_SH) < 0) {
        return fail_close(descriptor, error, error_size, "cannot lock file");
    }
    *file_fd = descriptor;
    return 0;
}

int fs_transfer_source(const fs_session *session, const char *path, int *file_fd,
                       char *filename, size_t filename_size,
                       char *error, size_t error_size)
{
    const char *slash = strrchr(path, '/');
    const char *leaf = slash == NULL ? path : slash + 1;

    /* A regular file cannot end with "." or "..", which name directories. */
    if (leaf[0] == '\0' ||
        snprintf(filename, filename_size, "%s", leaf) >= (int)filename_size) {
        return fail_with(error, error_size, EINVAL,
                         "invalid transfer source filename");
    }
    return fs_download_open(session, path, file_fd, error, error_size);
}

int fs_transfer_target(const fs_session *session, const char *directory,
                       const char *filename, int *file_fd, char *target_path,
                       size_t target_path_size, char *error, size_t error_size)
{
    char resolved_directory[PATH_MAX];
    struct stat status;
    int descriptor;

    if (filename[0] == '\0' || strchr(filename, '/') != NULL ||
        strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
        return fail_with(error, error_size, EINVAL, "invalid transfer filename");
    }
    if (fs_resolve_path(session, directory, 0, resolved_directory,
                        sizeof(resolved_directory), error, error_size) < 0) {
        return -1;
    }
    if (stat(resolved_directory, &status) < 0 || !S_ISDIR(status.st_mode)) {
        return fail_with(error, error_size, ENOTDIR,
                         "transfer destination is not a directory");
    }
    if (snprintf(target_path, target_path_size, "%s/%s",
                 resolved_directory, filename) >= (int)target_path_size) {
        return fail_with(error, error_size, ENAMETOOLONG,
                         "transfer destination path is too long");
    }
    descriptor = open(target_path,
                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (descriptor < 0) {
        return fail(error, error_size, "cannot create transfer destination");
    }
    if (flock(descriptor, LOCK_EX) < 0) {
        fail_close(descriptor, error, error_size, "cannot lock transfer destination");
        unlink(target_path);
        return -1;
    }
    *file_fd = descriptor;
    return 0;
}

int fs_transfer_finish(int file_fd, const char *target_path, int commit,
                       char *error, size_t error_size)
{
    int result = close(file_fd) < 0 && commit
                     ? fail(error, error_size, "cannot close transferred file") : 0;

    /* A failed transfer leaves no file behind. */
    if (!commit || result < 0) {
        unlink(target_path);
    }
    return result;
}
