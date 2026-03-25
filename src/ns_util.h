/*
 * Namespace util api for lau containers
 */
#ifndef _NS_UTIL_H_
#define _NS_UTIL_H_

#include <stdbool.h>

// lau error codes
#define LAU_EXIT_OK 1
#define LAU_OK   0
#define LAU_ERR  -1
#define LAU_EOF  -2 // read 0 on pipe
#define LAU_INTR -3 // intr
#define LAU_PIPE -4 // sig pipe error

#define STANDARD_MODE 0755

#define NETNS_DIR "/var/run/netns"
#define HOST_NETNS_PATH "/proc/self/ns/net"

#define RUN_CMD(x, ...) do { \
    int rc = run_cmd(x,  ##__VA_ARGS__) ; \
    if (rc != 0) return rc; \
} while(0);

#define RUN(x) do { \
    int rc = (x); \
    if (rc != 0) return rc; \
} while(0);



static inline int is_reaped(int status)
{
    return WIFEXITED(status) | WIFSIGNALED(status) ? 1 : 0;
}

static inline int close_fd(int *fd)
{
    int rc = 0;

    if (*fd != -1) {
        rc = close(*fd);
        *fd = -1;
    }

    return rc;
}

void shutdown_pid(int pid, int wait);

// sync pipe api
int sync_rdwr_close(int *rd_fd, int *wr_fd, 
    const char *who, const char *what, const char *name, 
    pid_t pid);

int sync_rdpipe(int *fd, 
    struct simple_sig *sig, 
    const char *who, const char *what, const char *name, 
    pid_t pid);

int sync_wrpipe(int *fd, 
    struct simple_sig *sig, 
    const char *who, const char *what, const char *name,
    pid_t pid);

// system run cmd wrapper
int run_cmd(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

char **exec_args_parse(const char *exec_path, const char *exec_args, int *argc);
char *gen_id(char *buf, int len, const char *name);
char *gen_path(const char *dir, const char *name);

int create_dir(const char *path, mode_t mode, bool can_exist);
int create_path_nocopy(char *path, mode_t mode);
int create_path(const char *path, mode_t mode);
char *create_subdir(const char *dir, const char *subdir, mode_t mode);
int copy_file(const char *src, const char *dst);
char *validate_dir(const char *key, const char *dir);

int create_netns_file(const char *netns_path);
int mount_netns(const char *netns_path);
int mount_file(const char *path);
int mount_cmd(const char *host_path, const char *rootfs_path);

int create_veth(const char *container, char *veth, int veth_len);
int setup_veth(const char *cont_name, const char *netns);

// child container
int set_identity(const char *name);
int set_rootfs(const char *rootfs);
int set_proc(void);
int create_network(const char *veth_name, const char *ip_addr);

int drop_bounding_set(const char *name);
int clear_all_caps(const char *name);
int drop_sudo(const char *name, uid_t uid, uid_t gid);
int drop_new_privs(const char *name);
int apply_seccomp(const char *name);

#endif
