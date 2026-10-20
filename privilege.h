#ifndef PRIVILEGE_H
#define PRIVILEGE_H

#include <sys/types.h>

/* When started as root, keep the saved UID 0 but run with an unprivileged euid. */
int privilege_init(void);
int privilege_raise(void);
int privilege_lower(void);
/* Permanently become the given user (all UIDs/GIDs are replaced). */
int privilege_become_user(const char *username, uid_t uid, gid_t gid);

#endif
