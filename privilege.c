#define _DEFAULT_SOURCE

#include "privilege.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>

static int privileged;
static uid_t service_uid;
static gid_t service_gid;

int privilege_init(void)
{
    struct passwd *nobody;

    if (geteuid() != 0) {
        return 0;
    }
    nobody = getpwnam("nobody");
    if (nobody == NULL) {
        errno = ENOENT;
        return -1;
    }
    service_uid = nobody->pw_uid;
    service_gid = nobody->pw_gid;
    privileged = 1;
    if (setgroups(0, NULL) < 0) {
        return -1;
    }
    return privilege_lower();
}

int privilege_raise(void)
{
    if (!privileged) {
        return 0;
    }
    if (seteuid(0) < 0 || setegid(0) < 0) {
        return -1;
    }
    return 0;
}

int privilege_lower(void)
{
    if (!privileged) {
        return 0;
    }
    if (setegid(service_gid) < 0 || seteuid(service_uid) < 0) {
        return -1;
    }
    return 0;
}

int privilege_become_user(const char *username, uid_t uid, gid_t gid)
{
    if (!privileged) {
        if (uid != geteuid()) {
            errno = EPERM;
            return -1;
        }
        return 0;
    }
    if (privilege_raise() < 0 || initgroups(username, gid) < 0 ||
        setgid(gid) < 0 || setuid(uid) < 0) {
        int saved_errno = errno;
        privilege_lower();
        errno = saved_errno;
        return -1;
    }
    privileged = 0;
    return 0;
}
