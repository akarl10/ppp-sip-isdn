#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#ifndef PPPD_PATH
#define PPPD_PATH "/usr/sbin/pppd"
#endif

#define SIP_PPP_PROCNAME "sip-isdn-pppd"

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

static int validate_pid(const char* pid, uid_t ruid, int* _pid)
{
    if(strlen(pid)==0)
        return 0;
    for (const char *p = pid; *p; ++p) {
        if (!isdigit((unsigned char)*p))
            return 0;
    }
    if(_pid) {        
        char *end;
        long p = strtol(pid, &end, 10);

        if (*end != '\0' || p <= 0 || p > INT_MAX)
            return 0;
        *_pid = p;
    }
    char pidpath [80];
    char cmdline [256];
    snprintf(pidpath, 80, "/proc/%s", pid);
    
    struct stat st;
    
    if (stat(pidpath, &st) != 0)
        return 0;
    
    if (st.st_uid != 0) // pppd is expected to run as root
        return 0;
    
    snprintf(pidpath, 80, "/proc/%s/exe", pid);
    
    memset(cmdline,0,sizeof(cmdline));
    ssize_t s = readlink(pidpath, cmdline, sizeof(cmdline)-1);
    if(s<0)
        return 0;
    cmdline[s]=0;

    if(strcmp(cmdline,PPPD_PATH)!=0)
        return 0;
    
    snprintf(pidpath, 80, "/proc/%s/cmdline", pid);
    int fd = open(pidpath,O_RDONLY,0);

    if(fd<0)
        return 0;
    memset(cmdline,0,sizeof(cmdline));
    s = read(fd,cmdline,sizeof(cmdline)-1);
    close(fd);
    if(s<0)
        return 0;
    cmdline[sizeof(cmdline)-1]=0;
    
    
    int procname_len = strlen(SIP_PPP_PROCNAME);
    // execv calles with argv[0] SIP_PPP_PROCNAME, +1 adds \0
    if(memcmp(cmdline,SIP_PPP_PROCNAME,procname_len + 1)!=0)
        return 0;

    if(memcmp(cmdline + procname_len + 1 ,"/dev/pts/",9)!=0)
        return 0;
    char * pts =  cmdline + procname_len + 1 + 9;
    
    return validate_pts(pts,cmdline,sizeof(cmdline),ruid);
    
}

int main(int argc, char **argv)
{
    uid_t ruid = getuid();
    
    if (argc == 3 ) {
        if(strcmp(argv[1],"stop")==0) {
            int pid = 0;
            if(validate_pid(argv[2],ruid,&pid)) {
                kill(pid,SIGHUP);
                return 0;
            }
            else {
                fprintf(stderr,"not allowed\n");
                return 1;
            }
        }
    }
    
    if (argc != 4) {
        fprintf(stderr,
                "usage: %s <pts#> <local-ip|''> <remote-ip|''>\n",
                argv[0]);
        fprintf(stderr,
                "usage: %s stop <pid>\n",
                argv[0]);
        return 1;
    }

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
        SIP_PPP_PROCNAME,
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

    if (setregid(0,0) != 0)
        exit(1);

    if (setreuid(0,0) != 0)
        exit(1);

    execve(PPPD_PATH, pppd_argv, envp);

    perror("execve");
    return 1;
}