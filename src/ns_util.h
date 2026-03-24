/*
 * Namespace util api for containers
 */
#ifndef _NS_UTIL_H_
#define _NS_UTIL_H_

#include <stdbool.h>

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


#define STANDARD_MODE 0755

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
