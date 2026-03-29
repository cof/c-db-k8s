/*
 * NameSpace Util API for containers
 * ---------------------------------
 * An api to manage namepsaces.
 *
 * API sections
 * ------------
 * Macros    : error codes and helpers:
 * Misc      : gen purpose helper funcs
 * Sync      : parent and child pipe sync
 * Dir       : create dir, copy file
 * netns     : open,create netns file
 * Mount     : mount overlay, rootfs cmd, file, netns
 * veth      : add,delete, setns, setup
 * namespace : child process namepace changes
 * security  : child process security changes
 * helpers   : status check, close func
 */
#ifndef _NS_UTIL_H_
#define _NS_UTIL_H_

#include <stdbool.h>

// lau error codes
#define LAU_EXIT_OK 1
#define LAU_OK   0
#define LAU_ERR  -1 // general error
#define LAU_EOF  -2 // read 0 on pipe
#define LAU_INTR -3 // intr
#define LAU_PIPE -4 // sig pipe error

#define STANDARD_MODE 0755

#define NETNS_DIR "/var/run/netns"
#define HOST_NETNS_PATH "/proc/self/ns/net"

// macro to run a system cmd and return if error
#define RUN_CMD(x, ...) do { \
    int rc = run_cmd(x,  ##__VA_ARGS__) ; \
    if (rc != 0) return rc; \
} while(0);

// macro to run a func and and return if error
#define RUN(x) do { \
    int rc = (x); \
    if (rc != 0) return rc; \
} while(0);

/*
 * Misc - gen purpose helper funcs
 * -------------------------------
 * run_cmd(fmt, ...)       : printf a cmd and call system to run it
 * shutdown_pid(pid, wait) : terminate a child process
 * exec_args_parse : convert cmd-line exec_args str into argv array
 * gen_id(buf, len, name)  : generate an id str based on hash of name
 * gen_path(dir, name)     : generate a path string 
 */
int run_cmd(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void shutdown_pid(int pid, int wait);
char **exec_args_parse(const char *exec_path, const char *exec_args, int *argc);
char *gen_id(char *buf, int len, const char *name);
char *gen_path(const char *dir, const char *name);

/*
 * Sync - parent and child pipe sync
 * ---------------------------------
 * sync_pipe_close(rd_fd, rw_fd, who, what, name, pid) : parent|child close its pipe end
 * sync_pipe_read(fd, sig, who, name pid)  : parent|child process reads from its pipe end
 * sync_pipe_write(fd, sig, who, name pid) : parent|child process writes to its pipe end
 */
int sync_pipe_close(int *rd_fd, int *wr_fd, 
    const char *who, const char *what, const char *name, 
    pid_t pid);

int sync_pipe_read(int *fd, 
    struct simple_sig *sig, 
    const char *who, const char *what, const char *name, 
    pid_t pid);

int sync_pipe_write(int *fd, 
    struct simple_sig *sig, 
    const char *who, const char *what, const char *name,
    pid_t pid);

/*
 * Dir - create dir
 * ----------------
 * create_dir(path, mode, can_exist) : mkdir with mode
 * create_path_nocopy(path, mode)    : mkdir -p with mode
 * create_path(path, mode)           : mkdir -p (copys path) 
 * create_path_for_file(file, mode)  : mkdir -p for file
 * create_subdir(dir, subdir, mode)  : mkdir subdir with mode
 */
int create_dir(const char *path, mode_t mode, bool can_exist);
int create_path_nocopy(char *path, mode_t mode);
int create_path(const char *path, mode_t mode);
int create_path_for_file(const char *file, mode_t mode);
char *create_subdir(const char *dir, const char *subdir, mode_t mode);

// copy file from src path to dst path
int copy_file(const char *src_path, const char *dst_path);
char *validate_dir(const char *key, const char *dir);

/*
 * netns 
 * -----
 * open_host_netns : open a fd to default host netns
 * switch_child_netns(fd, name) - switch to child netns
 * restore_host_netns(fd) : switch back to host netns
 * create_netns_file(path) : create a file for a netns
 */
int open_host_netns(void);
int switch_child_netns(int *fd, const char *name);
int restore_host_netns(int fd);
int create_netns_file(const char *netns_path);

/* Mount
 * ------
 * mount_overlay(path, lower,upper,work, who) : mount an OverlayFs
 * unmount_overlay(path, who) : unmount OverlayFS
 * mount_rootfs(rootfs_dir, rootfs_path) : mount a host rootfs_dir into container rootfs_path
 * mount_cmd(host_path, roofs_path) : mount a host binary into container filesystem - no file copy 
 * mount_file(path) : make a file a mount point
 * mount_netns(netns_path) : bind mount a new netns file - uses fork/exec to bind mount 
 */
int mount_overlay(const char *path, 
    const char *lowerdir, const char *upperdir, const char *workdir,
    const char *who);
int unmount_overlay(const char *path, const char *name);
int mount_file(const char *path);
int mount_cmd(const char *host_path, const char *rootfs_path);
int mount_netns(const char *netns_path);
int mount_rootfs(const char *rootfs_dir, const char *rootfs_path);

/* veth : add,delete, setns, setup 
 * --------------------------------
 * veth_add(veth,peer)     : create a new veth device
 * veth_del(veth)          : delete veth device
 * veth_setns(veth, netns) : set netns for veth
 * veth_gen_idstr(name, veth, veth_len, peer,peer_len) : generate id-str for veth
 * veth_setup(cont_name, netns) : create and setup a veth for container netns
 */
int veth_add(const char *veth, const char *peer);
int veth_del(const char *veth);
int veth_setns(const char *veth, const char *netns);
int veth_setup(const char *cont_name, const char *netns);

/* namespace : child process namepace changes
 * ------------------------------------------
 * set_identity(name) : set container hostname
 * set_rootfs(rootfs) : swith child process to new root file system
 * set_proc() : create proc dir
 * create_network(veth, ip_addr) : create veth, add addr, bring veth up
 */
int set_identity(const char *name);
int set_rootfs(const char *rootfs);
int set_proc(void);
int create_network(const char *veth_name, const char *ip_addr);

/*
 * security : child process security changes
 * -----------------------------------------
 * drop_bounding_set : drop all capabilities 
 * clear_all_caps : wipe existing capabilities
 * drop_sudo : drop sudo right
 * drop_new_privs : drop right to new privileges
 * apply_seccomp : apply syscall security filters  
 */
int drop_bounding_set(const char *name);
int clear_all_caps(const char *name);
int drop_sudo(const char *name, uid_t uid, uid_t gid);
int drop_new_privs(const char *name);
int apply_seccomp(const char *name);

/* helpers
 * -------
 * is_reaped(status) : true if status has exit or signal code
 * close_fd(fd)      : close fd if open
 */
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

#endif
