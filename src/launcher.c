/* 
 * launcher -a runtime container launcher
 * 
 *  CLONE_NEWUTS - private Hostname and NIS
 *  CLONE_PID    - private PID namespace
 *  CLONE_NEWNS  - privae mount namepsace
 *  CLONE_NEWNET - private network
 *
 * Notes
 * - TODO need to split this code into 3 (launcher/container/tools)
 *
 * Refs:
 * - man 7 nampspaces
 * - man 2 clone
 * - man 2 pivot_root
 * - man 2 wait
 * - Kerrisk - TLPI - The Linux Progamming Interface
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h> 
#include <string.h>
#include <stdarg.h>
#include <stddef.h>

#include <unistd.h>
#include <wordexp.h>
#include <libgen.h>
#include <errno.h>
#include <sched.h>
#include <limits.h>
#include <time.h>
#include <net/if.h>
#include <fcntl.h>
#include <grp.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/sendfile.h>
#include <sys/syscall.h> 

#include <sys/prctl.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/audit.h> 
#include <linux/seccomp.h>

#include "util.h"
#include "log.h"

// config defaults
#define RUN_DIR "/run/asimple_launcher"
#define STORE_dir "/var/lib/asimple_launcher"
#define NETNS_DIR "/var/run/netns"
#define HOST_NETNS_PATH "/proc/self/ns/net"

#define MAX_CONFIG 10
#define START_ORDER 1
#define START_DELAY 1

// security
#define DROP_SUDO  1
#define DROP_CAPS  1
#define DROP_PRIVS  1
#define USE_SECCOMP 1

#define EXIT_OK 1

// container config
struct myl_cnt {
    // user config
    char *name;      // container name
    char *cmd_path;  //  location of cmd
    char *exec_path;  // process to lanuch
    char **exec_argv; // command line args
    int exec_argc;
    char *ip_addr;    // ip addr to add to veth
    // paths
    char *root_path;     // location of container dir
    char *rootfs_path;   // For the bind mount and pivot_root()
    char *netns_path;    // bind mounted network namespae
    char *dst_path;      // bind mounted cmd path
    // networks
    char netns_name[IFNAMSIZ]; // network namespace name
    char veth_name[IFNAMSIZ];  // container eth0 link
    // used by overlay FS
    char *lower_path;   
    char *upper_path;   
    char *work_path;   
    int netns_fd;
    // parent sync child
    int go_read_fd;    // child reads
    int go_write_fd;   // parent writes
    // child sync with parent
    int ready_read_fd; // parent reads
    int ready_write_fd; // child writes
    // stack - created by mmap
    void *stack; 
    size_t stack_size;
    pid_t child_pid;  
    int status; // waitpid
    // security 
    uid_t uid;
    uid_t gid;
    // flags - bit fields
    unsigned int use_subdir      : 1; // use a rootfs subdir instead of name
    unsigned int need_network    : 1; // configure network
    unsigned int run             : 1; // clone child is active
    unsigned int netns_mounted   : 1; // netns active
    unsigned int overlay_mounted : 1; // overlay FS active
    unsigned int cmd_mounted     : 1; // cmd file was mounted
    unsigned int drop_sudo       : 1; // setuid|setgid
    unsigned int drop_caps       : 1; // drop capabilities 
    unsigned int drop_privs   : 1; // prctl PR_SET_NO_NEW_PRIVS
    unsigned int use_seccomp  : 1; // seccomp filter
    // waitpid flags
    unsigned int exit : 1;
    unsigned int signalled : 1;
};

// launcher state
struct myl_lau {
    char *cur_dir; // cwd where laucher start
    char *base_dir; // root dir for all launcher state
    char *src_dir; // where host cmd files live
    char *run_dir; // where host cmd files live
    char *netns_dir;  // netns mounts /var/run/netns
    char *runtime_dir; 
    char *store_dir;
    char *rootfs_dir; // where a rootfs lives
    char *netns_suffix;
    char *cable_prefix;
    int start_delay;
    // container config
    struct myl_cnt configs[MAX_CONFIG];
    unsigned int max_config;
    unsigned int num_config;
    // 
    unsigned int num_run;
    int host_netns_fd;
    mode_t dir_mode;
    // security
    char *sudo_user;
    int sudo_uid;
    int sudo_gid;
    int euid;
    // flags - bit fields
    unsigned int start_order  : 1; // start container in order
    unsigned int sudo_active  : 1; // launcher is run with sudo
    unsigned int drop_sudo    : 1; // drop sudo on containers
    unsigned int drop_caps    : 1; // drop capabilities
    unsigned int drop_privs   : 1; // prctl PR_SET_NO_NEW_PRIVS
    unsigned int use_seccomp  : 1; // use seccomp filters
    unsigned int use_name_id  : 1;
    unsigned int use_subdirs  : 1; // rootfs
    unsigned int use_overlay  : 1; // lower,upper,work,merged
    unsigned int mount_cmds   : 1; // mount cmd files instead of copying 
    unsigned int child_add_ip : 1; // child sets up network
};

static int verbose;

static inline int child_reaped(int status)
{
    return WIFEXITED(status) | WIFSIGNALED(status) ? 1 : 0;
}

static int run_cmd(const char *fmt, ...)
{
    va_list args;
    char *cmd;
    int rc;

    va_start(args, fmt);
    rc = vasprintf(&cmd, fmt, args);
    va_end(args);

    if (rc < 0) {
        return log_errno_rf("vsnprintf failed");
    }

    if (verbose) {
        log_info("%s", cmd);
    }

    rc = system(cmd);
    if (rc == -1) {
        log_errno("system(%s) failed", cmd);
    }
    else if (!WIFEXITED(rc)) {
        log_error("cmd (%s) interupted", cmd);
        rc = -1;
    }
    else if (WEXITSTATUS(rc) != 0) {
        log_error("cmd (%s) exited %d" , cmd, WEXITSTATUS(rc));
        rc = -1;
    }
    else {
        rc = 0;
    }

    free(cmd);

    // all done
    return rc; 
}


#define RUN_CMD(x, ...) do { \
    int rc = run_cmd(x,  ##__VA_ARGS__) ; \
    if (rc != 0) return rc; \
} while(0);

#define RUN(x) do { \
    int rc = (x); \
    if (rc != 0) return rc; \
} while(0);


#define STANDARD_MODE 0755

int create_dir(const char *path, mode_t mode, bool can_exist)
{
    int rc = mkdir(path, mode);

    if (rc == -1 && (!can_exist || errno != EEXIST)) {
        return log_errno_rf("create_dir %s failed", path);
    }

    return 0;
}

int create_path_nocopy(char *path, mode_t mode)
{
    if (!path) return -1;

    int len = strlen(path);
    if (path[len - 1] == '/') path[len - 1] = 0; 

    // now traverse the path string
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = 0; // Temporarily terminate string
            if (create_dir(path, mode, 1) != 0) {
                return -1;
            }
            *p = '/'; // Restore slash
        }
    }

    // Create last dir on path
    return create_dir(path, mode, 1);
}

int create_path(const char *path, mode_t mode)
{
    char *tmp = strdup(path);
    if (!tmp)
        return log_errno_rf("create_path strdup %s failed", path);

    int rc = create_path_nocopy(tmp, mode);
    free(tmp);

    return rc; 
}

static int mount_file(const char *path)
{
    // make file a mount point
    if (mount(path, path, "none", MS_BIND, NULL) < 0) {
        return log_errno_rf("mount self-bind %s failed", path);
    }

    // make file private
    if (mount("", path, NULL, MS_PRIVATE, NULL) < 0) {
        int _errno = errno;
        umount2(path, MNT_DETACH);
        errno = _errno;
        return log_errno_rf("mount private %s failed", path);
    }

    return 0;
}

static int create_netns_file(const char *netns_path)
{
    int fd = open(netns_path, O_RDONLY | O_CREAT | O_EXCL, 0600);

    if (fd == -1) {
        // error ?
        if (errno != EEXIST) {
            return log_errno_rf("open %s failed", netns_path);
        }
        // file alredy exists - possible crash ?
        if (umount2(netns_path, MNT_DETACH) == -1) {
            if (errno != EINVAL && errno != ENOENT) {
                return log_errno_rf("unmount2  %s failed", netns_path);
            }
        }
        // remove file
        if (unlink(netns_path) == -1) {
            return log_errno_rf("unlink stale  %s failed", netns_path);
        }
        // try again
        fd = open(netns_path, O_RDONLY | O_CREAT | O_EXCL, 0600);
        if (fd == -1) {
            return log_errno_rf("open %s failed", netns_path);
        }
    }

    if (close(fd) != 0) {
        // close failed -> rm file and return 
        int _errno = errno;
        unlink(netns_path);
        errno = _errno; 
        return log_errno_rf("close %s failed", netns_path);
    }

    return 0;
}

// bind mount a new nets
static int mount_netns(const char *netns_path)
{
    // create path
    if (create_netns_file(netns_path) != 0) {
        return -1;
    }

    // convert to mount point
    if (mount_file(netns_path) != 0) {
        unlink(netns_path);
        return -1;
    }

    // spawn a child for bind mount
    pid_t pid = fork();
    if (pid < 0) {
        int _errno = errno;
        umount2(netns_path, MNT_DETACH);
        unlink(netns_path);
        errno = _errno;
        return log_errno_rf("fork for bind mount failed");
    }

    // inside child - bind mount the new file to a new netns
    if (pid == 0) {
        // create new netns
        if (unshare(CLONE_NEWNET) < 0)  {
            log_errno("unshare create new-netns failed ");
            _exit(1);
        }
        // pin it bind
        if (mount(HOST_NETNS_PATH, netns_path, "none", MS_BIND, NULL) != 0) {
            log_errno("mount bind %s failed", netns_path);
            _exit(2);
        }
        // done
        _exit(0);
    }

    // parent 
    int status; 
    pid_t p = waitpid(pid, &status, 0);
    if (p == -1) {
        int _errno = errno;
        umount2(netns_path, MNT_DETACH);
        unlink(netns_path);
        errno = _errno;
        return log_errno_rf("waitpid failed");
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        int _errno = errno;
        umount2(netns_path, MNT_DETACH);
        unlink(netns_path);
        errno = _errno;
        return log_error_rf("failed to bind mount %s", netns_path);
    }

    // reopen netns 
    int netns_fd = open(netns_path, O_RDONLY | O_CLOEXEC);
    if (netns_fd == -1) {
        int _errno = errno;
        umount2(netns_path, MNT_DETACH);
        unlink(netns_path);
        errno = _errno;
        return log_errno_rf("reopen %s failed", netns_path);
    }

    if (umount2(netns_path, MNT_DETACH) < 0) {
        int _errno = errno;
        unlink(netns_path);
        errno = _errno;
        return log_errno_rf("reopen %s failed", netns_path);
    }

    // finally all done
    return netns_fd;
}

static int create_veth(const char *container, char *veth, int veth_len)
{
    uint32_t id = dbj2a_hash_str(container) & 0xffffffff;
    int rc, salt = 10;
    do {
        rc = snprintf(veth, veth_len, "vth%08x%d", id, salt);
        if (rc < 0)
            return log_errno_rf("snprintf: veth name failed");

        if (rc == 0 || rc >= veth_len)
            return log_error_rf("snprintf: incorrct len %d", rc);

        rc = run_cmd("ip link add %s type veth peer name %st 2>/dev/null", veth, veth);
        if (rc == 0) return 0;
    } while(--salt);

    // give up
    return -EEXIST;
}

int setup_veth(const char *cont_name, const char *netns)
{
    char veth[IFNAMSIZ];

    RUN(create_veth(cont_name, veth, sizeof(veth)));

    RUN_CMD("ip link set %st netns %s", veth, netns);
    RUN_CMD("ip link set %s up", veth);

    log_info("Created veth %s", veth);

    return 0;
}

static int set_identity(const char *name)
{
    int rc = sethostname(name, strlen(name));

    if (rc == -1)
        return log_errno_rf("sethostname %s failed", name);

    return 0;
}

// - make mount space private 
// - create new mount point
// - change dir to new mount point
// - pivot_root to new mount point (stack old_root)
// - change root dir to new mount
// - change curret dir to new root
// - umount/remove old root
static int set_rootfs(const char *rootfs)
{
    // - make mount space private 
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        return log_errno_rf("private mount failed");

    // - create new mount point
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) != 0)
        return log_errno_rf("bind mount roofs %s failed", rootfs);

    // - change dir to new mount point
    if (chdir(rootfs) != 0)
        return log_errno_rf("chdir rootfs %s", rootfs);

    // pivot_root to new mount point (stack old_root)
    if (syscall(SYS_pivot_root, ".", ".") == -1)
        return log_errno_rf("pivot_root failed");

    // - change root dir to new mount  task_struct (root ptr)
    if (chroot(".") != 0)
        return log_errno_rf("chroot failed");

    // - change curret dir to new root - task_struct (cwd ptr)
    if (chdir("/") != 0)
        return log_errno_rf("chdir to / failed");

    if (umount2("/", MNT_DETACH) != 0)
        return log_errno_rf("unmount2 / failed");

    // all done
    return 0;
}

static int set_proc(void)
{
    // new PID namespace - create new /proc
    if (create_dir("/proc", 0755, 1) != 0) {
        return log_errno_rf("mkdir /proc failed");
    }

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        return log_errno_rf("mount /proc faild");
    }

    return 0; 
}

// bring up child containers lo,eth0 and ip address
int create_network(const char *veth_name, const char *ip_addr)
{
    RUN_CMD("ip link set %s name eth0", veth_name); 
    RUN_CMD("ip addr add %s dev eth0", ip_addr);
    RUN_CMD("ip link set lo up");
    RUN_CMD("ip link set eth0 up");

    return 0;
}

static ssize_t read_sync(int fd)
{
    char buf;
    ssize_t nr;

    // block until parent writes to us
    do {
        nr = read(fd, &buf, 1);
    } while (nr == -1 && errno == EINTR);

    return nr;
}

static int write_sync(int fd)
{
    ssize_t nw;

    do {
        nw = write(fd, "!", 1);
    } while (nw == -1 && errno == EINTR);

    return nw == 1 ? 1 : -1;
}

static inline int close_fd(int *fd)
{
    int rc = 0;

    if (*fd != -1) {
        if (close(*fd) != 0) rc = -1;
        *fd = -1;
    }

    return rc;
}

static void shutdown_pid(int pid, int wait)
{
    int status;

    kill(pid, SIGTERM);

    if (waitpid(pid, &status, WNOHANG) == 0) {
        usleep(wait);
        if (waitpid(pid, &status, WNOHANG) == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }
}

static int child_wait_sync(struct myl_cnt *cnt)
{
    // close the pipe ends we don't need
    if (close_fd(&cnt->go_write_fd) != 0) {
        return log_errno_rf("close go_write for %s failed", cnt->name);
    }
    if (close_fd(&cnt->ready_read_fd) != 0) {
        return log_errno_rf("close ready_read for %s failed", cnt->name);
    }

    // read 1 byte from parent
    ssize_t nr = read_sync(cnt->go_read_fd);
    if (nr == -1) {
        return log_errno_rf("read-sync for %s failed", cnt->name);
    }
    if (nr != 1) {
        return log_error_rf("read-wait for %s got zero", cnt->name);
    }

    if (verbose) {
        log_info("Container (name=%s pid=%d) recv-go", cnt->name, cnt->child_pid);
    }

    return 0;;
}

static int child_wake_sync(struct myl_cnt *cnt)
{
    if (verbose)  {
        log_info("Container (name=%s pid=%d) send-ready", cnt->name, cnt->child_pid);
    }

    ssize_t nw = write_sync(cnt->ready_write_fd);

    if (nw == -1) {
        return log_errno_rf("write-ready_fd for %s failed", cnt->name);
    }
    if (nw != 1) {
        return log_error_rf("write-ready_fd for %s got zero", cnt->name);
    }

    // close pipe ends we no longer need
    if (close_fd(&cnt->ready_write_fd) != 0) {
        return log_errno_rf("close ready_write_fd for %s failed", cnt->name);
    }
    if (close_fd(&cnt->go_read_fd) != 0) {
        return log_errno_rf("close go_read_fd for %s failed", cnt->name);
    }

    return 0;;
}

int parent_close_go(struct myl_cnt *cnt)
{
    int num_err = 0;
    
    if (close_fd(&cnt->go_read_fd) != 0) {
        log_errno("parent close %s go_read_fd failed", cnt->name);
        num_err++;
    }

    if (close_fd(&cnt->ready_write_fd) != 0) {
        log_errno("parent close %s reay_write_fd failed", cnt->name);
        num_err++;
    }

    return num_err;
}

int create_pipes(struct myl_cnt *cnt)
{
    int fds[2];

    // create go sync pipe
    if (pipe(fds) == -1)  {
        return log_errno_rf("create go-pipe for %s failed", cnt->name);
    }

    cnt->go_read_fd = fds[0];
    cnt->go_write_fd = fds[1];

    if (pipe(fds) == -1) {
        return log_errno_rf("create ready_pipe for %s failed", cnt->name);
    }

    cnt->ready_read_fd = fds[0];
    cnt->ready_write_fd = fds[1];

    // all done
    return 0;
}

int copy_file(const char *src, const char *dst) 
{
    int rc = -1;

    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0)
        return log_errno_rf("copy_file open src %s failed", src);

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (dst_fd < 0) {
        close(src_fd);
        return log_errno_rf("copy_file open dst %s failed", dst);
    }

    struct stat stat_buf;
    if (fstat(src_fd, &stat_buf) == -1) {
        log_errno_rf("copy_file fstat src_fd %d failed", src_fd);
        goto close_all;
    }

    if (!S_ISREG(stat_buf.st_mode)) {
        log_errno_rf("copy_file src %s not a file", src);
        goto close_all;
    }   

    // XXX file size can be 0
    off_t offset = 0;
    while (offset < stat_buf.st_size) {
        ssize_t sent = sendfile(dst_fd, src_fd, &offset, stat_buf.st_size - offset);
        if (sent < 0) {
            if (errno == EINTR) continue; // Interrupted, try again
            log_errno_rf("copy_file send %s failed", src);
            goto close_all;
        }
        if (sent == 0) {
            log_error_rf("copy_file send %s eof", src);
            goto close_all;
        }
        // sendfile updates offset
    }

    // check all sent
    rc = offset == stat_buf.st_size ? 0 : -1;

close_all:
    close(src_fd);
    close(dst_fd);

    // check all sent
    return rc;
}

int mount_cmd_file(const char *host_path, const char *rootfs_path)
{
    // create an empty file - touch rootfs_path
    int fd = open(rootfs_path, O_CREAT | O_WRONLY, 0755);
    if (fd != -1) {
        return log_errno_rf("mount_cmd touch %s failed", rootfs_path);
    }
    close(fd); 

    // create a writable bind-mount
    if (mount(host_path, rootfs_path, NULL, MS_BIND, NULL) < 0) {
        int _errno = errno;
        unlink(rootfs_path);
        errno = _errno; 
        return log_errno_rf("mount_cmd bind mount %s failed", rootfs_path);
    }

    // update bind-mount to read-only
    if (mount(NULL, rootfs_path, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0) {
        int _errno = errno;
        log_errno_rf("mount_cmd remount read-only %s failed", rootfs_path);
        umount2(rootfs_path, MNT_DETACH); 
        unlink(rootfs_path);
        errno = _errno; 
        return log_errno_rf("mount_cmd bind mount %s failed", rootfs_path);
    }

    // all done
    return 0;
}

static inline char *get_rootfs(struct myl_cnt *cnt)
{
    return cnt->use_subdir ? cnt->rootfs_path : cnt->root_path;
}


int drop_bounding_set(void)
{
    for (int i = 0; i <= 63; i++) { 
        if (prctl(PR_CAPBSET_DROP, i, 0, 0, 0) == -1) {
            if (errno == EINVAL) break; 
            if (errno == EPERM) return -1;
        }
    }

    return 0;
}

int clear_all_caps(void)
{
    struct __user_cap_header_struct header = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct data[2] = { {0} };

    return syscall(SYS_capset, &header, data);
}

// samples/seccomp/bpf-direct.c
// strace -c server
int apply_seccomp(void)
{
    struct sock_filter filter[] = {

        //  Arch check (Required for security to prevent 32-bit bypass)
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (offsetof(struct seccomp_data, arch))),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64 , 1, 0), 
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

        //  Load syscall number
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (offsetof(struct seccomp_data, nr))),

        // Whitelist - Allow only what is strictly necessary
        // generated by gen_seccomp 
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_accept4, 34, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_arch_prctl, 33, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_bind, 32, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_brk, 31, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_close, 30, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_connect, 29, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_epoll_create1, 28, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_epoll_ctl, 27, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_epoll_wait, 26, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_execve, 25, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_exit_group, 24, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fcntl, 23, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_futex, 22, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getpid, 21, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getrandom, 20, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_ioctl, 19, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_listen, 18, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_lseek, 17, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mprotect, 16, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_newfstatat, 15, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_openat, 14, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_prlimit64, 13, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_read, 12, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_readlink, 11, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_recvfrom, 10, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_rseq, 9, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_rt_sigaction, 8, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_rt_sigreturn, 7, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_set_robust_list, 6, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_set_tid_address, 5, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_setsockopt, 4, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 3, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_uname, 2, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_write, 1, 0),

        // result
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),// if not matched
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)  // if matched
    };

    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter)/sizeof(filter[0])),
        .filter = filter,
    };

    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
}

static int drop_sudo(struct myl_cnt *cnt)
{
    /* needs musl-gcc or dynanic libs
    if (initgroups(cnt->user_name, cnt->gid) != 0) {
        return log_errno("initgroups %s failed", cnt->user_name);
    }
    */

    if (setgid(cnt->gid) != 0) {
        return log_errno_rf("setgid %d failed", cnt->gid);
    }

    if (setuid(cnt->uid) != 0) {
        return log_errno_rf("setuid %d failed", cnt->uid);
    }

    return 0;
}

static int setup_priv(struct myl_cnt *cnt)
{
    if (verbose) {
        log_info("Container (name=%s pid=%d) setup-priv (uid=%d,gid=%d)", 
            cnt->name, cnt->child_pid, cnt->uid, cnt->gid);
    }

    // drop all caps we can get
    if (cnt->drop_caps && drop_bounding_set() != 0) {
        return log_errno_rf("drop-boundin_set failed for container %s", cnt->name);
    }

    if (cnt->drop_sudo && drop_sudo(cnt) != 0) {
        return 0;
    }

    // drops all caps we have
    if (cnt->drop_caps && clear_all_caps() != 0)  {
        return log_errno_rf("clear_all_caps failed for container %s", cnt->name);
    }

    if (cnt->drop_privs && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1) {
        return log_errno_rf("prctl set-nonnew_privs failed for container %s", cnt->name);
    }

    if (cnt->use_seccomp && apply_seccomp() != 0) {
        return log_errno_rf("apply-seccomp failed for container %s", cnt->name);
    }

    return 0; 
}

// child process starts here
static int lau_cnt_start(void *arg)
{
    struct myl_cnt *cnt = arg;

    cnt->child_pid = getpid();
    if (verbose) {
        log_info("Container (name=%s pid=%d) started", cnt->name, cnt->child_pid);
    }

    if (set_identity(cnt->name) != 0) _exit(1);
    if (child_wait_sync(cnt) != 0) _exit(2);
    if (set_rootfs(get_rootfs(cnt)) !=0) _exit(3);
    if (set_proc() != 0) _exit(4);
    if (cnt->need_network && create_network(cnt->veth_name, cnt->ip_addr) != 0) _exit(5);
    if (child_wake_sync(cnt) != 0) _exit(6);

    // XXX close remaing fds other than stdio,stdout,stderr
    if (syscall(SYS_close_range, 3, ~0U, 0) == -1) {
        log_errno("close_range failed");
        _exit(7); 
    }

    if (setup_priv(cnt) != 0) _exit(8);

    // finally run the cmd
    execv(cnt->exec_path, cnt->exec_argv);
    log_errno("child %s execv '%s' failed", cnt->name, cnt->exec_path);
    _exit(9);
}

int lau_run_cnt(struct myl_lau *lau, struct myl_cnt *cnt)
{
    if (verbose) log_info("Launcher starting %s", cnt->name);

    if (create_pipes(cnt) != 0) {
        return -1;
    }

    // allocate a protected memory region for child stack
    // - never ever use malloc as child can corrupt it and parents heap
    // - linux stack grows downwards
    cnt->stack_size = 1024 * 1024;
    void *stack = mmap(NULL, cnt->stack_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0
    );
    if (stack == MAP_FAILED) {
        return log_errno_rf("mmap stack %zu failed", cnt->stack_size);
    }
    cnt->stack = stack; // XXX MAP_FAILED may not be 0

    // setup clone flags
    int clone_flags = SIGCHLD | CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS;
    if (cnt->netns_mounted) {
        //  switch to child netns
        if (setns(cnt->netns_fd, CLONE_NEWNET) != 0) {
            return log_errno_rf("netns-set(%d,'%s') for %s failed", cnt->netns_fd, cnt->netns_path, cnt->name);
        }
        if (close_fd(&cnt->netns_fd) != 0) {
            return log_errno_rf("close netns_fd for %s failed", cnt->name);
        }
    }
    else if (cnt->need_network) {
        clone_flags |= CLONE_NEWNET;
    }

    // launch child
    cnt->child_pid = clone(lau_cnt_start, cnt->stack + cnt->stack_size, clone_flags, cnt);
    if (cnt->child_pid == -1) {
        // failed ?
        int _errno = errno;
        // XXX retore the parnent netns
        setns(lau->host_netns_fd, CLONE_NEWNET);
        errno = _errno;
        return log_errno_rf("clone-child('%s','%s') failed", cnt->name, cnt->exec_path);
    }

    cnt->run = 1;
    lau->num_run++;

    // XXX retore the parents netns
    if (setns(lau->host_netns_fd, CLONE_NEWNET) != 0) {
        return log_errno_rf("set-netns %s failed", HOST_NETNS_PATH);
    }

    // close our ends - child has a copy
    if (parent_close_go(cnt) != 0) {
        return -1;
    }

    // release overlay mount
    if (cnt->overlay_mounted) {
        if (umount2(cnt->rootfs_path, MNT_DETACH) < 0 && errno != EINVAL) {
            log_errno("myl_cnt_run %s unmount overlay %s failed", cnt->name, cnt->rootfs_path);
        }
        cnt->overlay_mounted = 0;
        return -1;
    }

    // all done
    return 0;
}

void myl_cnt_cleanup(struct myl_cnt *cnt)
{
    if (cnt->run && cnt->child_pid > 0) {
        shutdown_pid(cnt->child_pid, 10000);
        cnt->run = 0;
    }

    // release all fds
    close_fd(&cnt->go_read_fd);
    close_fd(&cnt->go_write_fd);
    close_fd(&cnt->ready_read_fd);
    close_fd(&cnt->ready_write_fd);
    close_fd(&cnt->netns_fd);

    // release bind mount
    if (cnt->netns_mounted) {
        umount2(cnt->netns_path, MNT_DETACH);
        unlink(cnt->netns_path);
        cnt->netns_mounted = 0;
    }

    // release overlay mount
    if (cnt->overlay_mounted) {
        umount2(cnt->rootfs_path, MNT_DETACH);
        cnt->overlay_mounted = 0;
        if (cnt->cmd_mounted) {
            // XXX an rootfs unmout clears all mounts
            cnt->cmd_mounted = 0;
        }
    }

    // release cmd mount
    if (cnt->cmd_mounted) {
        umount2(cnt->dst_path, MNT_DETACH);
        cnt->cmd_mounted = 0;
    }

    // release stack memory
    if (cnt->stack) {
        munmap(cnt->stack, cnt->stack_size);
        cnt->stack = NULL;
    }

    // release name,exec_path,...
    if (cnt->name) free(cnt->name);
    if (cnt->cmd_path) free(cnt->cmd_path);
    if (cnt->exec_path) free(cnt->exec_path);
    if (cnt->exec_argc) {
        for (int i = 0; i < cnt->exec_argc; i++) {
            free(cnt->exec_argv[i]);
        }
        free(cnt->exec_argv);
    }
    if (cnt->ip_addr) free(cnt->ip_addr);

    if (cnt->root_path) free(cnt->root_path);
    if (cnt->rootfs_path) free(cnt->rootfs_path);
    if (cnt->netns_path) free(cnt->netns_path);
    if (cnt->dst_path) free(cnt->dst_path);

    if (cnt->lower_path) free(cnt->lower_path);
    if (cnt->upper_path) free(cnt->upper_path);
    if (cnt->work_path) free(cnt->work_path);

    // all done
}


static char **exec_args_parse(const char *exec_path, const char *exec_args, int *argc) 
{
    wordexp_t p = { 0 };

    if (exec_args && wordexp(exec_args, &p, WRDE_NOCMD) != 0) {
        log_error("wordexp failed");
        if (argc) *argc = 0;
        return NULL;
    }

    char **argv = malloc((p.we_wordc + 2) * sizeof(char *));
    if (argv == NULL) {
        log_errno("malloc failed");
        return NULL;
    }

    argv[0] = strdup(exec_path);
    if (!argv[0]) {
        log_errno("strdup failed");
        return NULL;
    }

    for (size_t i = 0; i < p.we_wordc; i++) {
        argv[i+1] = strdup(p.we_wordv[i]);
        if (!argv[i+1]) {
            log_errno("strdup failed");
            return NULL;
        }
    }

    argv[p.we_wordc + 1] = NULL; 
    if (argc) *argc = p.we_wordc + 1;

    wordfree(&p); 

    return argv;
}

int create_netns(struct myl_lau *lau, struct myl_cnt *cnt)
{
    // generate name e.g "name-ns"
    char *suffix = lau->netns_suffix ?: "";
    int rc = snprintf(cnt->netns_name, sizeof(cnt->netns_name), "%s%s", cnt->name, suffix);
    if (rc < 0) {
        return log_errno_rf("genname %s failed", cnt->name);
    }
    if ((size_t) rc >= sizeof(cnt->netns_name)) {
        return log_error_rf("genname %s no space", cnt->name);
    }

    // generate path e,g "/var/run/netns/name-ns"
    cnt->netns_path = gen_path(lau->netns_dir, cnt->netns_name);
    if (!cnt->netns_path) {
        return log_errno_rf("genpath %s failed", cnt->netns_name);
    }

    // bind mount path
    cnt->netns_fd = mount_netns(cnt->netns_path);
    if (cnt->netns_fd == -1) {
        return log_error_rf("mount_netns %s failed", cnt->netns_name);
    }
    cnt->netns_mounted = 1;

    log_info("Created network namespace: %s", cnt->netns_name);

    return 0;
}

static char *gen_id(char *buf, int len, const char *name)
{
    uint32_t id = dbj2a_hash_str(name) ^ (uint64_t) time(NULL);

    int rc = snprintf(buf, len, "%08x", id);
    if (rc < 0 || rc == 0 || rc >= len)  {
        return log_error_rn("gen_id failed for %s", name);
    }

    return buf;
}

char *create_subdir(const char *dir, const char *subdir, mode_t mode)
{
    char *path = gen_path(dir, subdir);
    if (!path) {
        return log_errno_rn("create_subdir genpath %s failed", subdir);
    }

    int rc = create_path(path, mode);
    if (rc != 0) {
        free(path);
        path = NULL;
    }

    return path;
}

int create_subdirs(struct myl_lau *lau, struct myl_cnt *cnt)
{
    if (!lau->use_subdirs) return 0;

    cnt->rootfs_path = create_subdir(lau->store_dir, "rootfs", 0755);
    if (!cnt->rootfs_path) return -1;

    // add overay
    if (!lau->use_overlay) return 0;
    cnt->lower_path = create_subdir(lau->store_dir, "lower", 0755);
    if (!cnt->lower_path) return -1;
    cnt->upper_path = create_subdir(lau->store_dir, "upper", 0755);
    if (!cnt->upper_path) return -1;
    cnt->work_path = create_subdir(lau->store_dir, "work", 0755);
    if (!cnt->work_path) return -1;

    return 0;
}

int create_root(struct myl_lau *lau, struct myl_cnt *cnt)
{
    char tmp[10];
    char *name;

    name = lau->use_name_id
        ? gen_id(tmp, sizeof(tmp), cnt->name) 
        : cnt->name;

    cnt->root_path = gen_path(lau->store_dir, name);
    if (!cnt->root_path) {
        return log_errno_rf("create_root genpath %s failed", name);
    }

    RUN(create_dir(cnt->root_path, lau->dir_mode, 1));

    return 0;
}

int mount_overlay(struct myl_lau *lau, struct myl_cnt *cnt)
{
    int rc;
    char *opts = NULL;

    if (!lau->use_overlay) return 0;

    rc = asprintf(&opts, 
        "lowerdir=%s,upperdir=%s,workdir=%s", 
        cnt->lower_path, cnt->upper_path, cnt->work_path
    );

    if (rc == -1) {
        return log_errno_rf("mount overlayfs genopts failed");
    }

    rc = mount("overlay", cnt->rootfs_path, "overlay", 0, opts);
    if (rc == -1) {
        log_errno("mount overlayfs %s failed", cnt->rootfs_path);
    }
    else {
        cnt->overlay_mounted = 1;
    }

    free(opts);

    return rc;
}

// copy files from host into container filesystem
int copy_files(struct myl_lau *lau, struct myl_cnt *cnt)
{
    if (verbose) {
        log_info("Launcher copy-files (name=%s, cmd=%s)", cnt->name, cnt->cmd_path);
    }

    char *cmd_path = NULL, *dst_path = NULL, *dst_dir = NULL;
    int rc = 0;

    // check src path exists
    cmd_path = realpath(cnt->cmd_path, NULL);
    if (!cmd_path) {
        log_errno("realpath %s failed", cnt->cmd_path);
        rc = -1;
        goto done;
    }

    // generate dst path - rootfs/exec_path
    dst_path = gen_path(get_rootfs(cnt), cnt->exec_path);
    if (!dst_path) {
        log_errno("gen_path %s failed", cnt->exec_path);
        rc = -1;
        goto done;
    }

    // need a copy of dst_path to parse
    dst_dir = strdup(dst_path);
    if (!dst_dir) {
        log_errno("strdup %s failed", dst_path);
        rc = -1;
        goto done;
    }

    // mkdir -p dst_dir
    rc = create_path_nocopy(dirname(dst_dir), lau->dir_mode);
    if (rc != 0) goto done;

    // finally copy or mount file 
    if (lau->mount_cmds) {
        rc = mount_cmd_file(cmd_path, dst_path);
        if (rc == 0) {
            cnt->cmd_mounted = 1;
            // need to store mount point
            cnt->dst_path = dst_path;
            dst_path = NULL;
        }
    }
    else {
        rc = copy_file(cmd_path, dst_path);
    }

done:
    if (cmd_path) free(cmd_path);
    if (dst_dir) free(dst_dir);
    if (dst_path) free(dst_path);

    return rc;
}

int check_network(struct myl_lau *lau, struct myl_cnt *cnt)
{
    if (cnt->ip_addr && lau->child_add_ip) { 
        cnt->need_network = 1;
    }

    return 0;
}

static int set_cable_name(struct myl_lau *lau, struct myl_cnt *cnt)
{
    const char *prefix = lau->cable_prefix ?: "";

    int rc = snprintf(cnt->veth_name, sizeof(cnt->veth_name), "%s%s", prefix, cnt->name);
    if (rc < 0) {
        return log_errno_rf("set_cable_name: snprintf failed");
    }
    if ((size_t) rc >= sizeof(cnt->veth_name)) {
        return log_error_rf("set_cable_name: no space");
    }

    return 0;
}

int setup_network(struct myl_cnt *cnt)
{
    if (verbose) {
        log_info("launcher setup-network (name=%s ipaddr=%s" , cnt->name, cnt->ip_addr);
    }

    RUN_CMD("nsenter -t %d -n ip link set %s name eth0", cnt->child_pid, cnt->veth_name);
    RUN_CMD("nsenter -t %d -n ip addr add %s/24 dev eth0", cnt->child_pid, cnt->ip_addr);
    RUN_CMD("nsenter -t %d -n ip link set lo up", cnt->child_pid);
    RUN_CMD("nsenter -t %d -n ip link set eth0 up", cnt->child_pid);

    cnt->need_network = 0;

    return 0;
}

int lau_check_reaped(struct myl_lau *lau, struct myl_cnt *cnt)
{
    char why[40];

    if (!child_reaped(cnt->status)) return 0;

    cnt->run = 0;
    lau->num_run--;

    if (WIFEXITED(cnt->status)) {
        int ec = WEXITSTATUS(cnt->status);
        snprintf(why, sizeof(why), "exit_code %d", ec);
        if (ec == 0) {
            log_info("Container '%s' exit ok (pid=%d why=%s)", cnt->name, cnt->child_pid, why);
            return EXIT_OK;
        }
        return log_error_rf("Container '%s' died (pid=%d why=%s)", cnt->name, cnt->child_pid, why);
    }
    else if (WIFSIGNALED(cnt->status)) {
        int sig = WTERMSIG(cnt->status);
        snprintf(why, sizeof(why), "signal %d (%s)", sig, strsignal(sig));
        return log_error_rf("Container '%s' died (pid=%d why=%s)", cnt->name, cnt->child_pid, why);
    }
    else {
        snprintf(why, sizeof(why), "status 0x%08x", cnt->status);
        return log_error_rf("Container '%s' died (pid=%d why=%s)", cnt->name, cnt->child_pid, why);
    }

}

int lau_check_wait(struct myl_lau *lau, struct myl_cnt *cnt)
{
    pid_t res = waitpid(cnt->child_pid, &cnt->status, WNOHANG);
    if (res == -1) {
        return log_errno_rf("waipid for %s failed", cnt->name);
    }
    if (res == cnt->child_pid && lau_check_reaped(lau, cnt) != 0) {
        return -1;
    }

    return 0;
}

int lau_wake_sync(struct myl_lau *lau, struct myl_cnt *cnt)
{
    // check if chlld still running
    if (lau_check_wait(lau, cnt) != 0) {
        return -1;
    }

    if (verbose) {
        log_info("Launcher (name=%s pid=%d) send-go", cnt->name, cnt->child_pid);
    }

    // wake up child
    ssize_t nw = write_sync(cnt->go_write_fd);
    if (nw != 1) {
        int _errno = errno;
        close_fd(&cnt->go_write_fd);
        errno = _errno;
        return log_errno_rf("write wake-sync for %s failed", cnt->name);
    }

    // relese pipe
    if (close_fd(&cnt->go_write_fd) != 0) {
        return log_errno_rf("close wait-sync for %s failed", cnt->name);
    }

    return 0;
}

int lau_wait_sync(struct myl_lau *lau, struct myl_cnt *cnt)
{
    // check if chlld still running
    if (lau_check_wait(lau, cnt) != 0) {
        return -1;
    }

    // wait for child
    ssize_t nr = read_sync(cnt->ready_read_fd);
    if (nr == -1)  {
        int _errno = errno;
        close_fd(&cnt->ready_read_fd);
        errno = _errno;
        return log_errno_rf("read wait-sync for %s failed", cnt->name);
    }

    // relese pipe
    if (close_fd(&cnt->ready_read_fd) != 0) {
        return log_errno_rf("close wait-sync %s failed", cnt->name);
    }

    if (verbose) {
        log_info("Launcher (name=%s pid=%d) recv-ready", cnt->name, cnt->child_pid);
    }

    return 0;
}

int lau_sync(struct myl_lau *lau)
{
    if (verbose) {
        log_info("Launcher sync %d containers %s", 
            lau->num_config,
            lau->start_order ? "sequential" : "parallel");
    }

    if (lau->start_order) {
        // sequential sync
        for (size_t i = 0; i < lau->num_config; i++) {
            RUN(lau_wake_sync(lau, &lau->configs[i]));
            RUN(lau_wait_sync(lau, &lau->configs[i]));
            sleep(lau->start_delay);
        }
    }
    else {
        // parallel sync
        for (size_t i = 0; i < lau->num_config; i++) {
            RUN(lau_wake_sync(lau, &lau->configs[i]));
        }
        for (size_t i = 0; i < lau->num_config; i++) {
            RUN(lau_wait_sync(lau, &lau->configs[i]));
        }
    }

    return 0;
}

// open host's default network interface
int lau_open_def_netns(struct myl_lau *lau)
{
    // XXX fd must be for this process only (O_CLOEXEC)
    lau->host_netns_fd = open(HOST_NETNS_PATH, O_RDONLY | O_CLOEXEC);
    if (lau->host_netns_fd == -1) {
        return log_errno_rf("open host_netns failed");
    }

    return 0;
}

// cable two containers together
int lau_cable(struct myl_lau *lau, struct myl_cnt *x, struct myl_cnt *y)
{
    if (verbose) {
        log_info("Launcher create-cable (left=%s, right=%s)", x->name, y->name);
    }

    RUN(set_cable_name(lau, x));
    RUN(set_cable_name(lau, y));

    RUN_CMD("ip link add %s type veth peer name %s", x->veth_name, y->veth_name);

    if (run_cmd("ip link set %s netns %d", x->veth_name, x->child_pid) != 0) {
        run_cmd("ip link del %s ", x->veth_name);
        return -1;
    }

    if (run_cmd("ip link set %s netns %d", y->veth_name, y->child_pid) != 0) {
        run_cmd("ip link del %s ", y->veth_name);
        return -1;
    }

    RUN(setup_network(x));
    RUN(setup_network(y));

    log_info("Created veth pair: %s <-> %s", x->veth_name, y->veth_name);

    return 0;
}

int lau_run(struct myl_lau *lau)
{
    if (verbose) {
        log_info("Launcher starting %d containers", lau->num_config);
    }

    for (size_t i = 0; i < lau->num_config; i++) {
        RUN(lau_run_cnt(lau, &lau->configs[i]));
    }

    return 0;
}

// setup infrastucture
int lau_setup(struct myl_lau *lau)
{
    if (verbose) {
        log_info("Launcher setup infrastucture");
    }

    RUN(lau_open_def_netns(lau));

    // create dirs
    RUN(create_path(lau->netns_dir, lau->dir_mode));
    RUN(create_path(lau->store_dir, lau->dir_mode));
    RUN(create_path(lau->run_dir, lau->dir_mode));

    for (size_t i = 0; i < lau->num_config; i++) {
        struct myl_cnt *cnt = &lau->configs[i];
        RUN(create_root(lau, cnt));
        RUN(create_subdirs(lau, cnt));
        RUN(mount_overlay(lau, cnt));
        RUN(copy_files(lau, cnt));
        RUN(check_network(lau, cnt));
        RUN(create_netns(lau, cnt));
    }

    // all done
    return 0;
}

static struct myl_cnt *lau_find_child(struct myl_lau *lau, pid_t pid)
{
    for (size_t i = 0; i < lau->num_config; i++) {
        if (lau->configs[i].child_pid == pid) {
            return &lau->configs[i];
        }
    }

    return NULL;
}

char *validate_dir(const char *key, struct str_slice dir) 
{
    char *path = realpath(dir.ptr, NULL);
    if (!path) {
        return log_errno_rn("%s realpath %s failed", key, dir.ptr);
    }

    // Check if the path exists
    int rc = -1;

    struct stat st;
    if (stat(path, &st) != 0) {
       log_error("%s path %s does not exist", key, path);
       goto done;
    }

    // check file is a directory
    if (!S_ISDIR(st.st_mode)) {
        log_error("%s path %s is not a dir", key, path);
        goto done;
    }

    // check dir is accesible
    if (access(path, R_OK | X_OK) != 0) {
        log_error("%s path %s is not accesible", key, path);
        goto done;
    }

    // okay
    rc = 0;

done:
    if (rc != 0) free(path);
    // all done
    return path;
}


static struct myl_cnt *lau_add(
    struct myl_lau *lau,
    const char *name, 
    const char *cmd_path,
    const char *exec_path, 
    const char *exec_args,
    const char *ip_addr)
{
    struct myl_cnt *cnt;

    if (!name) return log_error_rn("Missing container name");
    if (!cmd_path) return log_error_rn("Missing cmd_name");
    if (!exec_path) return log_error_rn("Missing exec_path");

    if (lau->num_config >= lau->max_config) { 
        return log_error_rn("Too many containers - num %d >= max %d", lau->num_config, lau->max_config);
    }

    cnt = &lau->configs[lau->num_config++];

    // init - XXX all fds must be set to -1
    memset(cnt, 0, sizeof(*cnt));
    cnt->go_read_fd  = -1;
    cnt->go_write_fd = -1;
    cnt->ready_read_fd = -1;
    cnt->ready_write_fd = -1;
    cnt->netns_fd = - 1;

    cnt->name = strdup(name);
    cnt->cmd_path = strdup(cmd_path);
    cnt->exec_path = strdup(exec_path);
    cnt->exec_argv = exec_args_parse(exec_path, exec_args, &cnt->exec_argc);

    if (ip_addr) {
        cnt->ip_addr = strdup(ip_addr);
    }

    // security
    if (lau->sudo_user && lau->drop_sudo) {
        cnt->drop_sudo = 1;
        cnt->uid = lau->sudo_uid;
        cnt->gid = lau->sudo_uid;
    }
    cnt->drop_caps = lau->drop_caps;
    cnt->drop_privs = lau->drop_privs;
    cnt->use_seccomp = lau->use_seccomp;

    return cnt;
}

// signal handling
volatile sig_atomic_t keep_running = 1;
volatile sig_atomic_t caught_signo = 0; 
volatile sig_atomic_t sender_pid = 0; 
volatile sig_atomic_t sender_uid = 0; 

int lau_wait(struct myl_lau *lau)
{
    int status = 0;

    while (keep_running && lau->num_run > 0) {
        pid_t pid = waitpid(-1, &status, 0); 
        if (pid == 0) continue;
        if (pid == -1) {
            // waitpid failed
            if (errno == EINTR) continue;
            if (errno == ECHILD) {
                // no more children - stop now
                for (size_t i = 0; i < lau->num_config; i++) {
                    lau->configs[i].run = 0;
                }
                lau->num_run = 0;
                break;
            }
            return log_errno_rf("waitpid failed");;
        }
        struct myl_cnt *cnt = lau_find_child(lau, pid);
        if (!cnt) {
            // XXX - not ours ?
            log_info("waitpid reaped unknown child pid %d", pid);
            continue;
        }
        // check if running 
        cnt->status = status;
        status = lau_check_reaped(lau, cnt);
        if (status != 0) {
            break;
        }
    }

    return status == EXIT_OK ? 0 : -1;
}

void lau_handle_signal(int signo, siginfo_t *info, void *ucontext)
{
    (void) ucontext;
    caught_signo = signo;

    sender_pid = 0;
    sender_uid = 0;

    if (info->si_code <= 0) {
        sender_pid = info->si_pid;
        sender_uid = info->si_uid;
    }

    keep_running = 0;
}

int lau_setup_signals(void)
{
    struct sigaction sa = { 0 };

    sa.sa_sigaction = lau_handle_signal;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        return log_errno_rf("setup sigint");
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        return log_errno_rf("setup sigterm");
    }

    // XXX prevent write(fd) trigger a SIGPIPE signal
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        return log_errno_rf("setup SIGPIPE");
    }

    // all done
    return 0;
}

void print_usage(struct myl_lau *lau, const char *cmd)
{
    (void) lau;
    const char *base = strrchr(cmd, '/');
    const char *prog_name = (base) ? base + 1 : cmd;
    int w= 15;

    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Options:\n");

    printf("  %-*s %s\n", w, "-help", "this help option");
    printf("  %-*s %s\n", w, "-v",    "debug verbose mode");
    printf("  %-*s %s\n", w, "rootfs=", "root file system");
    printf("  %-*s %s\n", w, "srcdir=", "cmd file dir");
    printf("  %-*s %s\n", w, "netnsdir=", "network namespace dir (default cwd)");
    printf("  %-*s %s default=%d\n", w, "startorder=","start containes order", START_ORDER);
    printf("  %-*s %s default=%d\n", w, "startdelay=","start delay order", START_DELAY);

    printf("  %-*s %s default=%d\n", w, "dropsudo=","drop sudo privilge", DROP_SUDO);
    printf("  %-*s %s default=%d\n", w, "dropcaps=","drop capabilities ", DROP_CAPS);
    printf("  %-*s %s default=%d\n", w, "dropprivs=","set  NO_NEW_PRIVS  ", DROP_PRIVS);
    printf("  %-*s %s default=%d\n", w, "useseccomp=", "use seccomp filters", USE_SECCOMP);

    printf("\nExample:\n");
    printf("  %s startorder=1 startdelay=5\n", prog_name);
}

int lau_parse_argv(struct myl_lau *lau, int argc, char *argv[])
{
    int num_err = 0;

    for (int i = 1; i < argc; i++) {
        // get key ["=value"]
        struct str_slice opt = slice_make_cstr(argv[i]);
        struct str_slice val = slice_split(&opt, '=');

        if (slice_cmp_cstr(opt, STR_LIT("-help"))) {
            print_usage(lau, argv[0]);
            num_err++;
        }
        else if (slice_cmp_cstr(opt, STR_LIT("-v"))) {
            // log everthtng
            verbose = 1;
        }
        else if (slice_cmp_cstr(opt, STR_LIT("rootfs"))) {
            lau->rootfs_dir = validate_dir("rootfs", val);
            if (!lau->rootfs_dir) {
                num_err++;
            }
        }
        else if (slice_cmp_cstr(opt, STR_LIT("srcdir"))) {
            lau->src_dir = validate_dir("srcdir", val);
            if (!lau->src_dir) {
                num_err++;
            }
        }
        else if (slice_cmp_cstr(opt, STR_LIT("netnsdir"))) {
            lau->netns_dir = validate_dir("netnsdir", val);
            if (!lau->netns_dir) {
                num_err++;
            }
        }
        else if (slice_cmp_cstr(opt, STR_LIT("startorder"))) {
            lau->start_order = atoi(val.ptr) != 0;
        }
        else if (slice_cmp_cstr(opt, STR_LIT("startdelay"))) {
            lau->start_delay = atoi(val.ptr);
            if (lau->start_delay < 0) {
                log_error("startdelay must greater than 0");
                num_err++;
            }
        }
        else if (slice_cmp_cstr(opt, STR_LIT("dropsudo"))) {
            lau->drop_sudo = atoi(val.ptr) != 0;
        }
        else if (slice_cmp_cstr(opt, STR_LIT("dropcaps"))) {
            lau->drop_caps = atoi(val.ptr) != 0;
        }
        else if (slice_cmp_cstr(opt, STR_LIT("dropprivs"))) {
            lau->drop_privs = atoi(val.ptr) != 0;
        }
        else if (slice_cmp_cstr(opt, STR_LIT("useseccomp"))) {
            lau->use_seccomp = atoi(val.ptr) != 0;
        } 
        else {
            log_errno("Unsupported option %s", opt.ptr);
            num_err++;
        }
    }

    return num_err;
}


int lau_init(struct myl_lau *lau)
{
    // set defaults
    lau->max_config = MAX_CONFIG;
    lau->start_delay = START_DELAY;
    lau->start_order = START_ORDER;

    // security
    lau->drop_sudo = DROP_SUDO;
    lau->drop_caps = DROP_CAPS;
    lau->drop_privs = DROP_PRIVS;
    lau->use_seccomp = USE_SECCOMP;

    // setup default dirs
    lau->cur_dir = getcwd(NULL, 0);
    if (!lau->cur_dir)  {
        return log_errno_rf("get_cwd failed");
    }

    lau->base_dir = gen_path(lau->cur_dir, "mylauncher");
    if (!lau->base_dir) {
        return log_errno_rf("gen_path mylaucher");
    }

    lau->netns_dir = gen_path(lau->base_dir, "netns_dir");
    lau->run_dir = gen_path(lau->base_dir, "run_dir");
    lau->store_dir = gen_path(lau->base_dir, "store_dir");
    lau->netns_suffix = strdup("-ns");
    lau->cable_prefix = strdup("veth-");
    lau->dir_mode = STANDARD_MODE;

    // get sudo
    char *env;
    if ((env = getenv("SUDO_USER")) != NULL) {
       lau->sudo_user = strdup(env);
    }
    if ((env = getenv("SUDO_UID")) != NULL) {
        lau->sudo_uid = atoi(env);
    }
    if ((env = getenv("SUDO_GID")) != NULL) {
        lau->sudo_gid = atoi(env);
    }
    lau->euid = geteuid();

    return 0;
}

void lau_destroy(struct myl_lau *lau)
{
    for (size_t i = 0; i < lau->num_config; i++) {
        myl_cnt_cleanup(&lau->configs[i]);
    }
    lau->num_config = 0;

    close_fd(&lau->host_netns_fd);

    if (lau->cur_dir) free(lau->cur_dir);
    if (lau->src_dir) free(lau->src_dir);

    if (lau->netns_dir) free(lau->netns_dir);
    if (lau->runtime_dir)  free(lau->runtime_dir);
    if (lau->store_dir) free(lau->store_dir);
    if (lau->rootfs_dir) free(lau->rootfs_dir);

    if (lau->netns_suffix) free(lau->netns_suffix);
    if (lau->cable_prefix) free(lau->cable_prefix);

    free(lau);
}

struct myl_lau *lau_create(void)
{
    struct myl_lau *lau;

    lau = malloc(sizeof(*lau));
    if (!lau) {
        return NULL;
    }

    // init
    memset(lau, 0, sizeof(*lau));
    lau->host_netns_fd = -1;

    return lau;
}

int main(int argc, char *argv[])
{
    int ec = -1;

    // create state
    struct myl_lau *lau = lau_create();
    if (!lau) fatal_error("Failed to create launher state");
    if (lau_init(lau) != 0) goto done;
    if (lau_parse_argv(lau, argc, argv) != 0) goto done;
    if (lau_setup_signals() != 0) goto done;

    // add containers
    struct myl_cnt *db  = lau_add(lau, "db", "db/server", "/bin/server", NULL, "10.0.0.1");
    struct myl_cnt *cli = lau_add(lau, "client", "client/client", "/bin/client", "10.0.0.1", "10.0.0.2");
    if (!db || !cli) goto done;

    // run containers
    if (lau_setup(lau) != 0) goto done;
    if (lau_run(lau) != 0) goto done;
    if (lau_cable(lau, cli, db) != 0) goto done;
    if (lau_sync(lau) != 0) goto done;
    if (lau_wait(lau) != 0) goto done;

    // no errors
    ec = 0;

done:   
    if (lau) lau_destroy(lau);

    return ec;
}
