#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int validate_ip_or_empty(const char *s)
{
    if (!s || *s == '\0')
        return 1;

    struct in_addr addr;

    return inet_pton(AF_INET, s, &addr) == 1;
}

static int validate_pts(const char *ptsnum,
                        char *path,
                        size_t pathlen,
                        uid_t ruid)
{
    if (!ptsnum || !*ptsnum)
        return 0;

    /* digits only */
    for (const char *p = ptsnum; *p; ++p) {
        if (!isdigit((unsigned char)*p))
            return 0;
    }

    snprintf(path, pathlen, "/dev/pts/%s", ptsnum);

    struct stat st;

    if (stat(path, &st) != 0)
        return 0;

    if (!S_ISCHR(st.st_mode))
        return 0;

    /* PTY must belong to caller */
    if (st.st_uid != ruid)
        return 0;

    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr,
                "usage: %s <pts#> <local-ip|''> <remote-ip|''>\n",
                argv[0]);
        return 1;
    }

    uid_t ruid = getuid();
    int ok = 0;

    char ptspath[64];

    if (!validate_pts(argv[1], ptspath, sizeof(ptspath), ruid)) {
        fprintf(stderr, "invalid pts\n");
        return 1;
    }

    if (!validate_ip_or_empty(argv[2])) {
        fprintf(stderr, "invalid local ip\n");
        return 1;
    }

    if (!validate_ip_or_empty(argv[3])) {
        fprintf(stderr, "invalid remote ip\n");
        return 1;
    }

    char localremote[64];

    snprintf(localremote,
             sizeof(localremote),
             "%s:%s",
             argv[2],
             argv[3]);

    char *pppd_argv[] = {
        "pppd",
        ptspath,
        localremote,
        "file",
        "/etc/ppp/options.isdn",
        "nodetach",
        NULL
    };

    /*
     * Minimal environment.
     */
    char *envp[] = {
        "PATH=/usr/sbin:/usr/bin:/bin",
        NULL
    };
    ok = setreuid(0,0);
    if(ok!=0)
        exit(1);
    ok = setregid(0,0);
    if(ok!=0)
        exit(1);

    execve("/usr/sbin/pppd", pppd_argv, envp);

    perror("execve");
    return 1;
}