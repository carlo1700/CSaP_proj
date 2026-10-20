#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "filesystem.h"
#include "protocol.h"

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

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error_size > 0) {
        snprintf(error, error_size, "%s: %s", message, strerror(errno));
    }
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

    if (username == NULL || username[0] == '\0' ||
        strlen(username) >= sizeof(((fs_session *)0)->username) ||
        username[0] == '.') {
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

int fs_initialize_root(const char *root_path, char *error, size_t error_size)
{
    char canonical_root[PATH_MAX];
    char users_path[PATH_MAX];
    struct stat status;
    int root_created = 0;

    if (mkdir(root_path, 0750) == 0) {
        root_created = 1;
    } else if (errno != EEXIST) {
        set_error(error, error_size, "cannot create root directory");
        return -1;
    }
    if (realpath(root_path, canonical_root) == NULL) {
        set_error(error, error_size, "cannot resolve root directory");
        return -1;
    }
    if (strcmp(canonical_root, "/") == 0) {
        errno = EINVAL;
        set_error(error, error_size, "server root cannot be the filesystem root");
        return -1;
    }
    if (geteuid() == 0) {
        struct group *shared_group = getgrnam("users");
        if (root_created && shared_group == NULL) {
            snprintf(error, error_size, "required shared system group 'users' is missing");
            return -1;
        }
        if (root_created && shared_group != NULL &&
            (chown(canonical_root, 0, shared_group->gr_gid) < 0 ||
             chmod(canonical_root, 0711) < 0)) {
            set_error(error, error_size, "cannot configure root directory");
            return -1;
        }
    }
    if (snprintf(users_path, sizeof(users_path), "%s/users", canonical_root) >=
        (int)sizeof(users_path)) {
        errno = ENAMETOOLONG;
        set_error(error, error_size, "root directory path is too long");
        return -1;
    }
    if (mkdir(users_path, 0750) < 0 && errno != EEXIST) {
        set_error(error, error_size, "cannot create users directory");
        return -1;
    }
    if (lstat(users_path, &status) < 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode)) {
        errno = ENOTDIR;
        set_error(error, error_size, "users path must be a directory");
        return -1;
    }
    if (geteuid() == 0) {
        struct group *shared_group = getgrnam("users");
        if (shared_group != NULL &&
            (chown(users_path, 0, shared_group->gr_gid) < 0 ||
             chmod(users_path, 0750) < 0)) {
            set_error(error, error_size, "cannot configure users directory");
            return -1;
        }
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
    pid_t child;
    pid_t waited;
    int child_status;

    if (!valid_username(username)) {
        snprintf(error, error_size, "invalid username");
        return -1;
    }
    if (geteuid() != 0) {
        snprintf(error, error_size,
                 "create_user requires the server to run with root privileges");
        return -1;
    }
    shared_group = getgrnam("users");
    if (shared_group == NULL) {
        snprintf(error, error_size, "required shared system group 'users' is missing");
        return -1;
    }
    if (getpwnam(username) != NULL) {
        snprintf(error, error_size, "a system user with that username already exists");
        return -1;
    }
    if (snprintf(user_path, sizeof(user_path), "%s/users/%s", root, username) >=
        (int)sizeof(user_path)) {
        errno = ENAMETOOLONG;
        set_error(error, error_size, "user path is too long");
        return -1;
    }
    if (lstat(user_path, &status) == 0) {
        snprintf(error, error_size, "a home directory with that username already exists");
        return -1;
    }
    if (errno != ENOENT) {
        set_error(error, error_size, "cannot check user home path");
        return -1;
    }
    child = fork();
    if (child < 0) {
        set_error(error, error_size, "cannot start adduser");
        return -1;
    }
    if (child == 0) {
        execlp("adduser", "adduser", "--disabled-password", "--gecos", "",
               "--ingroup", "users", username, (char *)NULL);
        _exit(127);
    }
    do {
        waited = waitpid(child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        set_error(error, error_size, "cannot wait for adduser");
        return -1;
    }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        snprintf(error, error_size, "adduser failed to create system account");
        return -1;
    }
    user = getpwnam(username);
    if (user == NULL || user->pw_gid != shared_group->gr_gid) {
        snprintf(error, error_size, "created account is not in the shared users group");
        return -1;
    }
    if (mkdir(user_path, permissions) < 0) {
        set_error(error, error_size, "cannot create user home");
        return -1;
    }
    if (chmod(user_path, permissions) < 0) {
        int saved_errno = errno;
        rmdir(user_path);
        errno = saved_errno;
        set_error(error, error_size, "cannot set home permissions");
        return -1;
    }
    if (lstat(user_path, &status) < 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode)) {
        errno = ENOTDIR;
        set_error(error, error_size, "user home is not a directory");
        return -1;
    }
    if (chown(user_path, user->pw_uid, shared_group->gr_gid) < 0 ||
        chmod(user_path, permissions) < 0) {
        set_error(error, error_size, "cannot set user home ownership or permissions");
        return -1;
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
        snprintf(error, error_size, "invalid username");
        return -1;
    }
    if (snprintf(user_path, sizeof(user_path), "%s/users/%s", root, username) >=
        (int)sizeof(user_path)) {
        errno = ENAMETOOLONG;
        set_error(error, error_size, "user home path is too long");
        return -1;
    }
    if (lstat(user_path, &status) < 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode)) {
        errno = ENOENT;
        set_error(error, error_size, "user does not exist");
        return -1;
    }
    user = getpwnam(username);
    if (user == NULL) {
        snprintf(error, error_size, "user has no corresponding system account");
        return -1;
    }
    if (status.st_uid != user->pw_uid) {
        errno = EACCES;
        set_error(error, error_size, "user home has the wrong system owner");
        return -1;
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
    size_t output_length;
    char *cursor;
    struct stat status;

    if (path == NULL || path[0] == '\0') {
        path = ".";
    }
    if (path[0] == '/') {
        if (snprintf(combined, sizeof(combined), "%s/%s", session->home,
                     path + 1) >= (int)sizeof(combined)) {
            errno = ENAMETOOLONG;
            set_error(error, error_size, "path is too long");
            return -1;
        }
    } else if (session->cwd[0] != '\0') {
        if (snprintf(combined, sizeof(combined), "%s/%s/%s", session->home,
                     session->cwd, path) >= (int)sizeof(combined)) {
            errno = ENAMETOOLONG;
            set_error(error, error_size, "path is too long");
            return -1;
        }
    } else if (snprintf(combined, sizeof(combined), "%s/%s", session->home,
                        path) >= (int)sizeof(combined)) {
        errno = ENAMETOOLONG;
        set_error(error, error_size, "path is too long");
        return -1;
    }

    if (strncmp(combined, session->home, home_length) != 0 ||
        (combined[home_length] != '/' && combined[home_length] != '\0')) {
        errno = EACCES;
        set_error(error, error_size, "path escapes user home");
        return -1;
    }

    if (snprintf(resolved, resolved_size, "%s", session->home) >=
        (int)resolved_size) {
        errno = ENAMETOOLONG;
        set_error(error, error_size, "path is too long");
        return -1;
    }
    output_length = strlen(resolved);
    cursor = combined + home_length;
    while (*cursor == '/') {
        ++cursor;
    }

    while (*cursor != '\0') {
        char *component = cursor;
        size_t component_length;
        int is_last;

        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        component_length = (size_t)(cursor - component);
        while (*cursor == '/') {
            ++cursor;
        }
        is_last = (*cursor == '\0');

        if (component_length == 0 ||
            (component_length == 1 && component[0] == '.')) {
            continue;
        }
        if (component_length == 2 && component[0] == '.' &&
            component[1] == '.') {
            if (output_length > home_length) {
                while (output_length > home_length &&
                       resolved[output_length - 1] != '/') {
                    --output_length;
                }
                if (output_length > home_length) {
                    --output_length;
                }
                resolved[output_length] = '\0';
            }
            continue;
        }

        if (output_length + component_length + 2 > resolved_size) {
            errno = ENAMETOOLONG;
            set_error(error, error_size, "path is too long");
            return -1;
        }
        resolved[output_length++] = '/';
        memcpy(resolved + output_length, component, component_length);
        output_length += component_length;
        resolved[output_length] = '\0';

        if (lstat(resolved, &status) < 0) {
            if (errno == ENOENT && allow_missing_leaf && is_last) {
                return 0;
            }
            set_error(error, error_size, "cannot access path");
            return -1;
        }
        if (S_ISLNK(status.st_mode)) {
            errno = EACCES;
            set_error(error, error_size, "symbolic links are not allowed");
            return -1;
        }
        if (!is_last && !S_ISDIR(status.st_mode)) {
            errno = ENOTDIR;
            set_error(error, error_size, "path component is not a directory");
            return -1;
        }
    }

    if (output_length == home_length) {
        resolved[home_length] = '\0';
    }
    return 0;
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
    if (directory) {
        if (mkdir(resolved, permissions) < 0 || chmod(resolved, permissions) < 0) {
            set_error(error, error_size, "cannot create directory");
            return -1;
        }
    } else {
        descriptor = open(resolved, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                          permissions);
        if (descriptor < 0) {
            set_error(error, error_size, "cannot create file");
            return -1;
        }
        if (fchmod(descriptor, permissions) < 0) {
            set_error(error, error_size, "cannot set file permissions");
            close(descriptor);
            return -1;
        }
        close(descriptor);
    }
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
        set_error(error, error_size, "cannot change permissions");
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
        errno = EBUSY;
        set_error(error, error_size, "cannot move user home");
        return -1;
    }
    /* Like mv: moving onto an existing directory places the source inside it. */
    if (stat(destination_path, &status) == 0 && S_ISDIR(status.st_mode)) {
        size_t length = strlen(destination_path);
        if (snprintf(destination_path + length, sizeof(destination_path) - length,
                     "/%s", strrchr(source_path, '/') + 1) >=
            (int)(sizeof(destination_path) - length)) {
            errno = ENAMETOOLONG;
            set_error(error, error_size, "destination path is too long");
            return -1;
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
        set_error(error, error_size, "cannot move path");
    }
    close_locked(destination_fd);
    close_locked(source_fd);
    return result;
}

static int append_text(char **buffer, size_t *length, size_t *capacity,
                       const char *text)
{
    size_t text_length = strlen(text);
    char *new_buffer;

    if (*length + text_length + 1 > PROTOCOL_MAX_FRAME) {
        errno = EOVERFLOW;
        return -1;
    }
    if (*length + text_length + 1 > *capacity) {
        size_t new_capacity = *capacity == 0 ? 1024 : *capacity;
        while (new_capacity < *length + text_length + 1) {
            new_capacity *= 2;
        }
        new_buffer = realloc(*buffer, new_capacity);
        if (new_buffer == NULL) {
            return -1;
        }
        *buffer = new_buffer;
        *capacity = new_capacity;
    }
    memcpy(*buffer + *length, text, text_length);
    *length += text_length;
    (*buffer)[*length] = '\0';
    return 0;
}

static int list_directory(const fs_session *session, const char *path,
                          char **output, size_t *output_length,
                          char *error, size_t error_size)
{
    char resolved[PATH_MAX];
    char line[PATH_MAX + 128];
    char *buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;
    DIR *directory;
    struct dirent *entry;

    if (fs_resolve_path(session, path, 0, resolved, sizeof(resolved),
                        error, error_size) < 0) {
        return -1;
    }
    directory = opendir(resolved);
    if (directory == NULL) {
        set_error(error, error_size, "cannot list directory");
        return -1;
    }

    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char entry_path[PATH_MAX];
        struct stat status;
        char permissions[10];
        int written;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(entry_path, sizeof(entry_path), "%s/%s", resolved,
                     entry->d_name) >= (int)sizeof(entry_path)) {
            errno = ENAMETOOLONG;
            set_error(error, error_size, "directory entry path is too long");
            free(buffer);
            closedir(directory);
            return -1;
        }
        if (lstat(entry_path, &status) < 0) {
            set_error(error, error_size, "cannot read directory entry");
            free(buffer);
            closedir(directory);
            return -1;
        }
        snprintf(permissions, sizeof(permissions), "%c%c%c%c%c%c%c%c%c",
                 status.st_mode & S_IRUSR ? 'r' : '-',
                 status.st_mode & S_IWUSR ? 'w' : '-',
                 status.st_mode & S_IXUSR ? 'x' : '-',
                 status.st_mode & S_IRGRP ? 'r' : '-',
                 status.st_mode & S_IWGRP ? 'w' : '-',
                 status.st_mode & S_IXGRP ? 'x' : '-',
                 status.st_mode & S_IROTH ? 'r' : '-',
                 status.st_mode & S_IWOTH ? 'w' : '-',
                 status.st_mode & S_IXOTH ? 'x' : '-');
        written = snprintf(line, sizeof(line), "%s\t%s\t%lld bytes\n",
                           permissions, entry->d_name,
                           (long long)status.st_size);
        if (written < 0 || (size_t)written >= sizeof(line) ||
            append_text(&buffer, &length, &capacity, line) < 0) {
            errno = EOVERFLOW;
            set_error(error, error_size, "directory listing is too large");
            free(buffer);
            closedir(directory);
            return -1;
        }
        errno = 0;
    }
    if (errno != 0) {
        set_error(error, error_size, "cannot read directory");
        free(buffer);
        closedir(directory);
        return -1;
    }
    closedir(directory);

    if (buffer == NULL) {
        buffer = calloc(1, 1);
        if (buffer == NULL) {
            return -1;
        }
    }
    *output = buffer;
    *output_length = length;
    return 0;
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
        errno = ENAMETOOLONG;
        set_error(error, error_size, "path is too long");
        return -1;
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
    fs_session other_session = *session;
    struct stat status;

    if (!valid_username(username)) {
        snprintf(error, error_size, "invalid username in list path");
        return -1;
    }
    if (snprintf(other_session.home, sizeof(other_session.home),
                 "%s/users/%s", session->root, username) >=
        (int)sizeof(other_session.home)) {
        errno = ENAMETOOLONG;
        set_error(error, error_size, "user home path is too long");
        return -1;
    }
    if (lstat(other_session.home, &status) < 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode)) {
        errno = ENOENT;
        set_error(error, error_size, "user home does not exist");
        return -1;
    }
    other_session.cwd[0] = '\0';
    return list_directory(&other_session, path, output, output_length,
                          error, error_size);
}

int fs_write(const fs_session *session, const char *path, off_t offset,
             const unsigned char *data, size_t data_length,
             char *error, size_t error_size)
{
    char resolved[PATH_MAX];
    int descriptor;
    size_t written = 0;
    int replace = offset < 0;

    if (replace) {
        offset = 0;
    }
    if (fs_resolve_path(session, path, 1, resolved, sizeof(resolved),
                        error, error_size) < 0) {
        return -1;
    }
    descriptor = open(resolved, O_WRONLY | O_CREAT | O_NOFOLLOW,
                      0700);
    if (descriptor < 0) {
        set_error(error, error_size, "cannot open file for writing");
        return -1;
    }
    if (flock(descriptor, LOCK_EX) < 0 ||
        (replace && ftruncate(descriptor, 0) < 0) ||
        lseek(descriptor, offset, SEEK_SET) < 0) {
        set_error(error, error_size, "cannot lock or seek file for writing");
        close(descriptor);
        return -1;
    }
    while (written < data_length) {
        ssize_t count = write(descriptor, data + written, data_length - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            set_error(error, error_size, "cannot write file");
            close(descriptor);
            return -1;
        }
        written += (size_t)count;
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
        errno = EBUSY;
        set_error(error, error_size, "cannot delete user home");
        return -1;
    }
    if (lstat(resolved, &status) < 0) {
        set_error(error, error_size, "cannot access path to delete");
        return -1;
    }
    if (S_ISDIR(status.st_mode)) {
        if (rmdir(resolved) < 0) {
            set_error(error, error_size, "cannot delete directory");
            return -1;
        }
        return 0;
    }
    /* Deleting is a write: wait for readers and writers to finish. */
    descriptor = open_locked(resolved, LOCK_EX);
    result = unlink(resolved);
    if (result < 0) {
        set_error(error, error_size, "cannot delete file");
    }
    close_locked(descriptor);
    return result;
}

int fs_change_directory(fs_session *session, const char *path,
                        char *error, size_t error_size)
{
    char resolved[PATH_MAX];
    struct stat status;
    size_t home_length;

    if (fs_resolve_path(session, path, 0, resolved, sizeof(resolved),
                        error, error_size) < 0) {
        return -1;
    }
    if (stat(resolved, &status) < 0 || !S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        set_error(error, error_size, "not a directory");
        return -1;
    }
    home_length = strlen(session->home);
    if (resolved[home_length] == '\0') {
        session->cwd[0] = '\0';
    } else {
        snprintf(session->cwd, sizeof(session->cwd), "%s",
                 resolved + home_length + 1);
    }
    return 0;
}

int fs_upload_begin(const fs_session *session, const char *path, int *file_fd,
                    char *error, size_t error_size)
{
    struct stat status;
    int descriptor;
    char resolved[PATH_MAX];

    if (fs_resolve_path(session, path, 1, resolved, sizeof(resolved),
                        error, error_size) < 0) {
        return -1;
    }
    if (strcmp(resolved, session->home) == 0) {
        errno = EISDIR;
        set_error(error, error_size, "upload destination must be a file path");
        return -1;
    }
    descriptor = open(resolved, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK,
                      0600);
    if (descriptor < 0) {
        set_error(error, error_size, "cannot open upload destination");
        return -1;
    }
    if (fstat(descriptor, &status) < 0 || !S_ISREG(status.st_mode)) {
        errno = EINVAL;
        set_error(error, error_size, "upload destination must be a regular file");
        close(descriptor);
        return -1;
    }
    if (flock(descriptor, LOCK_EX) < 0 || ftruncate(descriptor, 0) < 0 ||
        lseek(descriptor, 0, SEEK_SET) < 0) {
        set_error(error, error_size, "cannot lock or prepare upload destination");
        close(descriptor);
        return -1;
    }
    *file_fd = descriptor;
    return 0;
}

int fs_upload_finish(int file_fd, int commit,
                     char *error, size_t error_size)
{
    int result = 0;

    if (!commit && ftruncate(file_fd, 0) < 0) {
        set_error(error, error_size, "cannot clear incomplete upload");
        result = -1;
    }
    if (close(file_fd) < 0) {
        if (result == 0) {
            set_error(error, error_size, "cannot close uploaded file");
        }
        result = -1;
    }
    if (!commit && result == 0) {
        errno = EIO;
        set_error(error, error_size, "upload did not complete");
        result = -1;
    }
    return result;
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
        set_error(error, error_size, "cannot open file for download");
        return -1;
    }
    if (fstat(descriptor, &status) < 0 || !S_ISREG(status.st_mode)) {
        errno = EINVAL;
        set_error(error, error_size, "download source must be a regular file");
        close(descriptor);
        return -1;
    }
    if (flock(descriptor, LOCK_SH) < 0) {
        set_error(error, error_size, "cannot lock file for download");
        close(descriptor);
        return -1;
    }
    *file_fd = descriptor;
    return 0;
}

int fs_transfer_source(const fs_session *session, const char *path, int *file_fd,
                       char *filename, size_t filename_size,
                       char *error, size_t error_size)
{
    char resolved[PATH_MAX];
    const char *leaf;
    int descriptor;

    if (fs_resolve_path(session, path, 0, resolved, sizeof(resolved),
                        error, error_size) < 0) {
        return -1;
    }
    leaf = strrchr(resolved, '/');
    leaf = leaf == NULL ? resolved : leaf + 1;
    if (leaf[0] == '\0' || strcmp(leaf, ".") == 0 ||
        strcmp(leaf, "..") == 0 ||
        snprintf(filename, filename_size, "%s", leaf) >= (int)filename_size) {
        errno = EINVAL;
        set_error(error, error_size, "invalid transfer source filename");
        return -1;
    }
    descriptor = open(resolved, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) {
        set_error(error, error_size, "cannot open transfer source");
        return -1;
    }
    {
        struct stat status;
        if (fstat(descriptor, &status) < 0 || !S_ISREG(status.st_mode)) {
            errno = EINVAL;
            set_error(error, error_size, "transfer source must be a regular file");
            close(descriptor);
            return -1;
        }
    }
    if (flock(descriptor, LOCK_SH) < 0) {
        set_error(error, error_size, "cannot lock transfer source");
        close(descriptor);
        return -1;
    }
    *file_fd = descriptor;
    return 0;
}

int fs_transfer_target(const fs_session *session, const char *directory,
                       const char *filename, int *file_fd, char *target_path,
                       size_t target_path_size, char *error, size_t error_size)
{
    char resolved_directory[PATH_MAX];
    struct stat status;
    int descriptor;

    if (filename == NULL || filename[0] == '\0' ||
        strchr(filename, '/') != NULL ||
        strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
        errno = EINVAL;
        set_error(error, error_size, "invalid transfer filename");
        return -1;
    }
    if (fs_resolve_path(session, directory, 0, resolved_directory,
                        sizeof(resolved_directory), error, error_size) < 0) {
        return -1;
    }
    if (stat(resolved_directory, &status) < 0 || !S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        set_error(error, error_size, "transfer destination is not a directory");
        return -1;
    }
    if (snprintf(target_path, target_path_size, "%s/%s",
                 resolved_directory, filename) >= (int)target_path_size) {
        errno = ENAMETOOLONG;
        set_error(error, error_size, "transfer destination path is too long");
        return -1;
    }
    descriptor = open(target_path,
                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK,
                      0600);
    if (descriptor < 0) {
        set_error(error, error_size, "cannot create transfer destination");
        return -1;
    }
    if (flock(descriptor, LOCK_EX) < 0) {
        set_error(error, error_size, "cannot lock transfer destination");
        close(descriptor);
        unlink(target_path);
        return -1;
    }
    *file_fd = descriptor;
    return 0;
}

int fs_transfer_finish(int file_fd, const char *target_path, int commit,
                       char *error, size_t error_size)
{
    int result = 0;

    if (!commit && ftruncate(file_fd, 0) < 0) {
        set_error(error, error_size, "cannot clear incomplete transfer");
        result = -1;
    }
    if (close(file_fd) < 0) {
        if (result == 0) {
            set_error(error, error_size, "cannot close transferred file");
        }
        result = -1;
    }
    if (!commit || result < 0) {
        if (unlink(target_path) < 0 && errno != ENOENT && result == 0) {
            set_error(error, error_size, "cannot remove incomplete transfer");
            result = -1;
        }
    }
    return result;
}
