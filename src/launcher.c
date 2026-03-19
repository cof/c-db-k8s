/* 
 * launcher : runtime container launcher
 * Usage    : ./launcher --help
 * Example  : sudo ./laucher
 * 
 * Notes
 * - CLONE_NEWUTS - private Hostname and NIS
 * - CLONE_PID    - private PID namespace
 * - CLONE_NEWNS  - privae mount namepsace
 * - CLONE_NEWNET - private network
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

#include "util.h"
#include "log.h"
#include "ns_util.h"

// config defaults
#define BASE_DIR "mylauncher"
#define RUN_DIR "/run/asimple_launcher"
#define STORE_dir "/var/lib/asimple_launcher"

#define MAX_CONFIG 10
#define START_ORDER 1
#define START_DELAY 1

// security
#define DROP_SUDO  1
#define DROP_CAPS  1
#define DROP_PRIVS  1
#define USE_SECCOMP 1

// lau error codes
#define LAU_RECV_INTR  1
#define LAU_CHILD_OK   2
#define LAU_CHILD_ERR -1

// container config
struct myl_child {
    // user config
    char *name;      // container name
    char *cmd_path;  //  location of cmd
    char *exec_path;  // process to lanuch
    char **exec_argv; // command line args
    int exec_argc;
    char *ip_addr;    // ip addr to add to veth
    // paths
    char *store_dir;     // location of container dir
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
    pid_t pid;  
    int clone_flags;
    int status; // waitpid
    // security 
    uid_t uid;
    uid_t gid;
    // flags - bit fields
    unsigned int use_subdir       : 1; // use a rootfs subdir instead of name
    unsigned int need_network     : 1; // configure network
    unsigned int run              : 1; // clone child is active
    unsigned int storedir_created : 1; // store_dir created
    unsigned int rootfs_created   : 1; // rootfs_dir created
    unsigned int netns_mounted    : 1; // netns active
    unsigned int rootfs_mounted   : 1; // rootfs_dir mounted
    unsigned int overlay_mounted  : 1; // overlay FS active
    unsigned int cmd_mounted      : 1; // cmd file was mounted
    unsigned int drop_sudo        : 1; // setuid|setgid
    unsigned int drop_caps        : 1; // drop capabilities 
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
    int (*sync_all)(struct myl_lau *lau);
    // container config
    struct myl_child configs[MAX_CONFIG];
    int max_config;
    int num_config;
    // 
    unsigned int num_run;
    int host_netns_fd;
    mode_t dir_mode;
    pid_t pid;
    // security
    char *sudo_user;
    int sudo_uid;
    int sudo_gid;
    int euid;
    // flags - bit fields
    unsigned int need_basedir : 1; // create base dir
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

// signal handling
volatile sig_atomic_t keep_running = 1;
volatile sig_atomic_t caught_signo = 0; 
volatile sig_atomic_t sender_pid = 0; 
volatile sig_atomic_t sender_uid = 0; 


static inline char *child_get_rootfs(struct myl_child *child)
{
    return child->use_subdir ? child->rootfs_path : child->store_dir;
}

/* child process code */
static int setup_priv(struct myl_child *child)
{
    if (verbose) {
        log_info("LOG", "Container (name=%s pid=%d) setup-priv (uid=%d,gid=%d)", 
            child->name, child->pid, child->uid, child->gid);
    }

    if (child->drop_caps && drop_bounding_set(child->name)) return -1;
    if (child->drop_sudo && drop_sudo(child->name, child->uid , child->gid)) return -1;
    if (child->drop_caps && clear_all_caps(child->name)) return -1;
    if (child->drop_privs && drop_new_privs(child->name))  return -1;
    if (child->use_seccomp && apply_seccomp(child->name)) return -1;

    return 0; 
}

static int child_send_ready(struct myl_child *child)
{
    if (verbose)  {
        log_info("LOG", "Container (name=%s pid=%d) send-ready", child->name, child->pid);
    }

    // wake up parent
    while (write(child->ready_write_fd, "!", 1) == -1)  {
        if (errno == EINTR) {
            if (!keep_running) return LAU_RECV_INTR;
            continue;
        }
        return log_errno_rf("child %s write send-ready failed", child->name);
    }

    // close pipe ends we no longer need
    if (close_fd(&child->ready_write_fd) != 0) {
        return log_errno_rf("chile %s close send-ready failed", child->name);
    }

    return 0;;
}

static int child_wait_go(struct myl_child *child)
{
    // close the pipe ends we don't need
    if (close_fd(&child->go_write_fd) != 0) {
        return log_errno_rf("close go_write for %s failed", child->name);
    }
    if (close_fd(&child->ready_read_fd) != 0) {
        return log_errno_rf("close ready_read for %s failed", child->name);
    }

    // wait for parent
    ssize_t nr;
    char ch;
    while ((nr = read(child->go_read_fd, &ch, 1)) == -1) {
        if (errno == EINTR) {
            if (!keep_running) return LAU_RECV_INTR;
            continue;
        }
        return log_errno_rf("child %s read wait-go failed", child->name);
    }

    // release pipe
    if (close_fd(&child->go_read_fd) != 0) {
        return log_errno_rf("child %s close send-go failed", child->name);
    }

    if (verbose) {
        log_info("LOG", "Container (name=%s pid=%d) recv-go", child->name, child->pid);
    }

    return 0;;
}

// child process starts here
static int lau_cnt_start(void *arg)
{
    struct myl_child *child = arg;

    child->pid = getpid();
    if (verbose) {
        log_info("LOG", "Container (name=%s pid=%d) started", child->name, child->pid);
    }

    if (set_identity(child->name) != 0) _exit(1);
    if (child_wait_go(child) != 0) _exit(2);
    if (set_rootfs(child_get_rootfs(child)) !=0) _exit(3);
    if (set_proc() != 0) _exit(4);
    if (child->need_network && create_network(child->veth_name, child->ip_addr) != 0) _exit(5);
    if (child_send_ready(child) != 0) _exit(6);

    // XXX close remaing fds other than stdio,stdout,stderr
    if (syscall(SYS_close_range, 3, ~0U, 0) == -1) {
        log_errno("child %s pid=%d close_range failed", child->name, child->pid);
        _exit(7); 
    }

    if (setup_priv(child) != 0) _exit(8);

    // finally run the cmd
    execv(child->exec_path, child->exec_argv);
    log_errno("child %s execv '%s' failed", child->name, child->exec_path);
    _exit(9);
}

/* wait pids code */
static int lau_check_reaped(struct myl_lau *lau, struct myl_child *child, int status)
{
    child->status = status;

    if (!is_reaped(child->status)) return 0;

    child->run = 0;
    lau->num_run--;

    int rc = LAU_CHILD_ERR;

    char why[40];
    if (WIFEXITED(child->status)) {
        int exit_code = WEXITSTATUS(child->status);
        snprintf(why, sizeof(why), "exit %d", exit_code);
        if (exit_code == 0) {
            log_info("+", "Container '%s' exit ok (pid=%d why=%s)", child->name, child->pid, why);
            return LAU_CHILD_OK;
        }
    }
    else if (WIFSIGNALED(child->status)) {
        int sig = WTERMSIG(child->status);
        snprintf(why, sizeof(why), "signal %d (%s)", sig, strsignal(sig));
    }
    else {
        snprintf(why, sizeof(why), "unknown 0x%08x", child->status);
    }

    return log_error_re(rc, "Container '%s' reaped (pid=%d why=%s)", child->name, child->pid, why);
}

static struct myl_child *lau_find_child(struct myl_lau *lau, pid_t pid)
{
    for (int i = 0; i < lau->num_config; i++) {
        if (lau->configs[i].pid == pid) {
            return &lau->configs[i];
        }
    }

    return NULL;
}

// wait for intr or child exits
static int lau_wait_pids(struct myl_lau *lau)
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
                for (int i = 0; i < lau->num_config; i++) {
                    lau->configs[i].run = 0;
                }
                lau->num_run = 0;
                break;
            }
            return log_errno_rf("waitpid failed");;
        }
        struct myl_child *child = lau_find_child(lau, pid);
        if (!child) {
            log_info("LOG", "waitpid reaped unknown pid %d", pid);
            continue;
        }
        // check if child stll running 
        status = lau_check_reaped(lau, child, status);
        if (status != 0) break;
    }

    return status == LAU_CHILD_OK ? 0 : -1;
}

/* sync all code */

// check child is still running
static int lau_check_running(struct myl_lau *lau, struct myl_child *child)
{
    int status;

    pid_t res = waitpid(child->pid, &status, WNOHANG);
    if (res == -1) {
        return log_errno_rf("waipid for %s failed", child->name);
    }
    if (res == child->pid && lau_check_reaped(lau, child, status) != 0) {
        return -1;
    }

    return 0;
}


// tell child it can go
static int lau_send_go(struct myl_lau *lau, struct myl_child *child)
{
    if (lau_check_running(lau, child) != 0) {
        return -1;
    }

    if (verbose) {
        log_info("LOG", "Launcher (name=%s pid=%d) send-go", child->name, child->pid);
    }

    // wake up child
    while (write(child->go_write_fd, "!", 1) == -1)  {
        if (errno == EINTR) {
            if (!keep_running) return LAU_RECV_INTR;
            continue;
        }
        return log_errno_rf("lau send-go %s failed", child->name);
    }

    // release pipe
    if (close_fd(&child->go_write_fd) != 0) {
        return log_errno_rf("close send-go for %s failed", child->name);
    }

    return 0;
}

// wait for child to become ready
static int lau_wait_ready(struct myl_lau *lau, struct myl_child *child)
{
    if (lau_check_running(lau, child) != 0) {
        return -1;
    }

    if (verbose) {
        log_info("LOG", "Launcher (name=%s pid=%d) wait-ready", child->name, child->pid);
    }

    // wait for child
    ssize_t nr;
    char ch;
    while ((nr = read(child->ready_read_fd, &ch, 1)) == -1) {
        if (errno == EINTR) {
            if (!keep_running) return LAU_RECV_INTR;
            continue;
        }
        return log_errno_rf("lau wait-ready %s failed", child->name);
    }

    // relese pipe
    if (close_fd(&child->ready_read_fd) != 0) {
        return log_errno_rf("close wait-ready %s failed", child->name);
    }

    if (verbose) {
        log_info("LOG", "Launcher (name=%s pid=%d) recv-ready", child->name, child->pid);
    }

    return 0;
}

// sequential start - i.e. ensure DB server is up before client
static int lau_sync_inorder(struct myl_lau *lau) 
{
    if (verbose) {
        log_info("LOG", "Launcher sync %d containers in order", lau->num_config);
    }

    struct myl_child *child;
    int rc;

    for (int i = 0; i < lau->num_config; i++) {
        child = &lau->configs[i];
        rc = lau_send_go(lau, child);
        if (rc) return rc;
        rc = lau_wait_ready(lau, child);
        if (rc) return rc;
        sleep(lau->start_delay);
    }

    return 0;
}

// parallel start - note clone/fork start order is undefined by OS
static int lau_sync_parallel(struct myl_lau *lau) 
{
    if (verbose) {
        log_info("LOG", "Launcher sync %d containers in parallel", lau->num_config);
    }

    for (int i = 0; i < lau->num_config; i++) {
        int rc = lau_send_go(lau, &lau->configs[i]);
        if (rc) return rc;
    }

    for (int i = 0; i < lau->num_config; i++) {
        int rc = lau_wait_ready(lau, &lau->configs[i]);
        if (rc) return rc;
    }

    return 0;
}

// create child process to be the container
static int lau_run_child(struct myl_lau *lau, struct myl_child *child)
{
    if (verbose) {
        log_info("LOG", "Launcher starting %s", child->name);
    }

    if (child->netns_mounted) {
        // switch to child netns before clone
        if (setns(child->netns_fd, CLONE_NEWNET) != 0) {
            return log_errno_rf("run-child %s setns(%d,'%s') failed", 
                child->name, child->netns_fd, child->netns_path);
        }
        if (close_fd(&child->netns_fd) != 0) {
            int _errno = errno;
            // restore host netns
            setns(lau->host_netns_fd, CLONE_NEWNET);
            errno = _errno;
            return log_errno_rf("run-child %s close netns_fd %d failed", child->name, child->netns_fd);
        }
    }

    // launch child (fork/exec)
    child->pid = clone(lau_cnt_start, child->stack + child->stack_size, child->clone_flags, child);
    if (child->pid == -1) {
        // failed ?
        int _errno = errno;
        // restore host netns
        setns(lau->host_netns_fd, CLONE_NEWNET);
        errno = _errno;
        return log_errno_rf("run-child %s exec '%s' clone failed", child->name, child->exec_path);
    }

    // clean up after run
    child->run = 1;
    lau->num_run++;

    int num_err = 0;

    // restore host netns
    if (child->netns_mounted && setns(lau->host_netns_fd, CLONE_NEWNET) != 0) {
        log_errno_rf("run-child %s restore netns %s failed", child->name, HOST_NETNS_PATH);
        num_err++;
    }

    // close our ends - child has a copy
    if (close_fd(&child->go_read_fd) != 0) {
        log_errno("parent close %s go_read_fd failed", child->name);
        num_err++;
    }

    if (close_fd(&child->ready_write_fd) != 0) {
        log_errno("parent close %s reay_write_fd failed", child->name);
        num_err++;
    }

    // release overlay mount
    if (child->overlay_mounted) {
        if (umount2(child->rootfs_path, MNT_DETACH) < 0 && errno != EINVAL) {
            log_errno("myl_child_run %s unmount overlay %s failed", child->name, child->rootfs_path);
            num_err++;
        }
        child->overlay_mounted = 0;
    }

    // done
    return num_err == 0 ? 0 : -1;
}

// create pipe / stack / clone flags
static int child_prepare(struct myl_child *child)
{
    int fds[2];

    // create go sync pipe
    if (pipe(fds) == -1)  {
        return log_errno_rf("create go-pipe for %s failed", child->name);
    }
    child->go_read_fd = fds[0];
    child->go_write_fd = fds[1];

    // create ready sync pipe
    if (pipe(fds) == -1) {
        return log_errno_rf("create ready_pipe for %s failed", child->name);
    }
    child->ready_read_fd = fds[0];
    child->ready_write_fd = fds[1];

    // allocate a protected memory region for child stack
    // - never ever use malloc as child can corrupt it and parents heap
    // - linux stack grows downwards
    child->stack_size = 1024 * 1024;
    void *stack = mmap(NULL, child->stack_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0
    );
    if (stack == MAP_FAILED) {
        return log_errno_rf("child_prepare %s create stack %zu failed", child->name, child->stack_size);
    }
    child->stack = stack; // XXX MAP_FAILED may not be 0

    // setup clone flags
    child->clone_flags = SIGCHLD | CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS;
    if (child->need_network) {
        child->clone_flags |= CLONE_NEWNET;
    }

    return 0;
}

static int lau_start_all(struct myl_lau *lau)
{
    if (verbose) {
        log_info("LOG", "Starting %d containers", lau->num_config);
    }

    for (int i = 0; i < lau->num_config; i++) {
        int rc = child_prepare(&lau->configs[i]);
        if (rc) return rc;
    }

    for (int i = 0; i < lau->num_config; i++) {
        int rc = lau_run_child(lau, &lau->configs[i]);
        if (rc) return rc;
    }

    return 0;
}

static int child_setup_network(struct myl_child *child)
{
    if (verbose) {
        log_info("LOG", "launcher setup-network (name=%s ipaddr=%s" , child->name, child->ip_addr);
    }

    RUN_CMD("nsenter -t %d -n ip link set %s name eth0", child->pid, child->veth_name);
    RUN_CMD("nsenter -t %d -n ip addr add %s/24 dev eth0", child->pid, child->ip_addr);
    RUN_CMD("nsenter -t %d -n ip link set lo up", child->pid);
    RUN_CMD("nsenter -t %d -n ip link set eth0 up", child->pid);

    child->need_network = 0;

    return 0;
}

static int set_cable_name(struct myl_lau *lau, struct myl_child *child)
{
    const char *prefix = lau->cable_prefix ?: "";

    int rc = snprintf(child->veth_name, sizeof(child->veth_name), "%s%s", prefix, child->name);
    if (rc < 0) {
        return log_errno_rf("set_cable_name: snprintf failed");
    }
    if ((size_t) rc >= sizeof(child->veth_name)) {
        return log_error_rf("set_cable_name: no space");
    }

    return 0;
}

static int lau_link_veths(struct myl_lau *lau, struct myl_child *x, struct myl_child *y)
{
    if (verbose) {
        log_info("LOG", "Launcher create-cable (left=%s, right=%s)", x->name, y->name);
    }

    RUN(set_cable_name(lau, x));
    RUN(set_cable_name(lau, y));

    RUN_CMD("ip link add %s type veth peer name %s", x->veth_name, y->veth_name);

    if (run_cmd("ip link set %s netns %d", x->veth_name, x->pid) != 0) {
        run_cmd("ip link del %s ", x->veth_name);
        return -1;
    }

    if (run_cmd("ip link set %s netns %d", y->veth_name, y->pid) != 0) {
        run_cmd("ip link del %s ", y->veth_name);
        return -1;
    }

    RUN(child_setup_network(x));
    RUN(child_setup_network(y));

    log_info("+", "Created veth pair: %s <-> %s", x->veth_name, y->veth_name);

    return 0;
}

static int check_network(struct myl_lau *lau, struct myl_child *child)
{
    if (child->ip_addr && lau->child_add_ip) { 
        child->need_network = 1;
    }

    return 0;
}

static int create_netns(struct myl_lau *lau, struct myl_child *child)
{
    // generate name e.g "name-ns"
    char *suffix = lau->netns_suffix ?: "";
    int rc = snprintf(child->netns_name, sizeof(child->netns_name), "%s%s", child->name, suffix);
    if (rc < 0) {
        return log_errno_rf("genname %s failed", child->name);
    }
    if ((size_t) rc >= sizeof(child->netns_name)) {
        return log_error_rf("genname %s no space", child->name);
    }

    // generate path e,g "/var/run/netns/name-ns"
    child->netns_path = gen_path(lau->netns_dir, child->netns_name);
    if (!child->netns_path) {
        return log_errno_rf("genpath %s failed", child->netns_name);
    }

    // bind mount path
    child->netns_fd = mount_netns(child->netns_path);
    if (child->netns_fd == -1) {
        return log_error_rf("mount_netns %s failed", child->netns_name);
    }
    child->netns_mounted = 1;

    log_info("+", "Created network namespace: %s", child->netns_name);

    return 0;
}

// load cmd file from host into container filesystem
static int load_cmd(struct myl_lau *lau, struct myl_child *child)
{
    if (verbose) {
        log_info("LOG", "Launcher load-cmd (name=%s, cmd=%s dst=%s)", 
            child->name, child->cmd_path,  child->exec_path);
    }

    int rc = 1;
    char *src_path, *dst_path, *dst_dir;
    src_path = dst_path = dst_dir = NULL;

    // Get absoulte cmd-path
    char *cmd_path = child->cmd_path;
    if (!cmd_path) {
        return log_error_rf("copy-cmd %s missing cmd_path", child->name);
    }
    if (*cmd_path != '/') {
        // need full path
        src_path = gen_path(lau->src_dir, child->cmd_path);
        if (!src_path) goto done;
        cmd_path = src_path;
    }

    // Build dst-path - storedir/child/rootfs/exec_path
    dst_path = gen_path(child_get_rootfs(child), child->exec_path);
    if (!dst_path) goto done;

    // need a copy of dst_path to parse
    dst_dir = strdup(dst_path);
    if (!dst_dir) {
        log_errno("copy-cmd %s strdup dst_path failed", child->name);
        goto done;
    }
    dst_dir = dirname(dst_dir);

    // create dst_dir (mkdir -p dst_dir)
    rc = create_path_nocopy(dst_dir, lau->dir_mode);
    if (rc) goto done;

    // mount or copy the file into container store-dir
    if (lau->mount_cmds) {
        rc = mount_cmd(cmd_path, dst_path);
        if (rc == 0) {
            child->cmd_mounted = 1;
            // need to store mount point
            child->dst_path = dst_path;
            dst_path = NULL;
        }
    }
    else {
        rc = copy_file(cmd_path, dst_path);
    }

done:
    if (src_path) free(src_path);
    if (dst_dir)  free(dst_dir);
    if (dst_path) free(dst_path);

    return rc;
}

static int mount_overlay(struct myl_lau *lau, struct myl_child *child)
{
    if (!lau->use_overlay) return 0;

    if (verbose) {
        log_info("LOG", "Launcher mount-overlay (name=%s)", child->name);
    }

    char *opts = NULL;
    int rc = asprintf(&opts, 
        "lowerdir=%s,upperdir=%s,workdir=%s", 
        child->lower_path ?: lau->rootfs_dir,
        child->upper_path, 
        child->work_path
    );

    if (rc == -1) {
        return log_errno_rf("mount overlayfs genopts failed");
    }

    rc = mount("overlay", child->rootfs_path, "overlay", 0, opts);
    if (rc == -1) {
        log_errno("mount overlayfs %s failed", child->rootfs_path);
    }
    else {
        child->overlay_mounted = 1;
    }

    free(opts);

    return rc;
}

static int mount_rootfs(struct myl_lau *lau, struct myl_child *child)
{
    if (!lau->use_subdirs) return 0;
    if (!lau->rootfs_dir) return 0;
    if (lau->use_overlay) return 0;

    // TODO overlay works but not this ?
    if (mount(lau->rootfs_dir, child->rootfs_path, NULL, MS_BIND, NULL) != 0) {
        return log_errno_rf("mount bind rootfs %s to %s failed", lau->rootfs_dir, child->rootfs_path);
    }
    child->rootfs_mounted = 1;
   
    // make all furither moutns private
    if (mount(NULL, child->rootfs_path, NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        return log_errno_rf("mount private %s failed", child->rootfs_path);
    }


    if (verbose) {
        log_info("LOG", "Launcher mount-rootfs (name=%s rootfs=%s)", 
            child->name, child->rootfs_path);
    }

    return 0;
}

static int create_subdirs(struct myl_lau *lau, struct myl_child *child)
{
    if (!lau->use_subdirs) return 0;

    // create store-dir/rootfs
    child->rootfs_path = create_subdir(child->store_dir, "rootfs", 0755);
    if (!child->rootfs_path) return -1;
    child->rootfs_created = 1;
    child->use_subdir = 1;

    // add overlay
    if (!lau->use_overlay) return 0;

    if (!lau->rootfs_dir) {
        // need store-dir/child-name/lower
        child->lower_path = create_subdir(child->store_dir, "lower", 0755);
        if (!child->lower_path) return -1;
    }

    // store-dir/child-name/upper
    child->upper_path = create_subdir(child->store_dir, "upper", 0755);
    if (!child->upper_path) return -1;

    // store-dir/child-name/work
    child->work_path = create_subdir(child->store_dir, "work", 0755);
    if (!child->work_path) return -1;

    return 0;
}

// create folder for container files
static int create_storedir(struct myl_lau *lau, struct myl_child *child)
{
    char tmp[10];
    char *name;

    // create store-dir/child-name
    name = lau->use_name_id
        ? gen_id(tmp, sizeof(tmp), child->name) 
        : child->name;

    child->store_dir = gen_path(lau->store_dir, name);
    if (!child->store_dir) {
        return log_errno_rf("create_storedir %s genpath failed", name);
    }
    RUN(create_dir(child->store_dir, lau->dir_mode, 1));
    child->storedir_created = 1;

    if (verbose) {
        log_info("LOG", "Launcher create-storedir (name=%s storedir=%s)", 
            child->name, child->store_dir);
    }

    return 0;
}


static int check_rootfs_dir(struct myl_lau *lau)
{
    if (verbose) {
        log_info("LOG", "Launcher check-rootfs-dir");
    }

    if (!lau->rootfs_dir) return 0;

    /* Do we need to make  rootfs_dir private 
    if (mount(lau->rootfs_dir, lau->rootfs_dir, NULL, MS_BIND | MS_REC, NULL) != 0) {
        return log_errno_rf("mount bind %s failed", lau->rootfs_dir);
    }
    if (mount(NULL, lau->rootfs_dir, NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        return log_errno_rf("mount private %s failed", lau->rootfs_dir);
    }
    */

    lau->use_subdirs = 1;
    lau->use_overlay = 1;

    return 0;
}

// open host's default network namespace
static int lau_open_host_netns(struct myl_lau *lau)
{
    // fd must be for this process only (O_CLOEXEC)
    lau->host_netns_fd = open(HOST_NETNS_PATH, O_RDONLY | O_CLOEXEC);
    if (lau->host_netns_fd == -1) {
        return log_errno_rf("open host_netns %s failed", HOST_NETNS_PATH);
    }

    return 0;
}

// setup infrastucture
static int lau_setup(struct myl_lau *lau)
{
    if (verbose) {
        log_info("LOG", "Launcher setup infrastucture");
    }


    RUN(lau_open_host_netns(lau));
    
    if (lau->need_basedir) {
        RUN(create_path(lau->base_dir, lau->dir_mode));
    }

    // create dirs
    RUN(create_dir(lau->netns_dir, lau->dir_mode, 1));
    RUN(create_dir(lau->store_dir, lau->dir_mode, 1));
    RUN(create_dir(lau->run_dir, lau->dir_mode, 1));

    RUN(check_rootfs_dir(lau));

    for (int i = 0; i < lau->num_config; i++) {
        struct myl_child *cnt = &lau->configs[i];
        RUN(create_storedir(lau, cnt));
        RUN(create_subdirs(lau, cnt));
        RUN(mount_rootfs(lau, cnt));
        RUN(mount_overlay(lau, cnt));
        RUN(load_cmd(lau, cnt));
        RUN(check_network(lau, cnt));
        RUN(create_netns(lau, cnt));
    }

    // all done
    return 0;
}


static struct myl_child *lau_add(
    struct myl_lau *lau,
    const char *name, 
    const char *cmd_path,
    const char *exec_path, 
    const char *exec_args,
    const char *ip_addr)
{
    if (!name) return log_error_rn("Missing container name");
    if (!cmd_path) return log_error_rn("Missing cmd_name");
    if (!exec_path) return log_error_rn("Missing exec_path");

    if (lau->num_config >= lau->max_config) { 
        return log_error_rn("Too many containers. num-config %d >= max %d", lau->num_config, lau->max_config);
    }

    struct myl_child *child = &lau->configs[lau->num_config++];
    memset(child, 0, sizeof(*child));

    // init all fds to -1
    child->go_read_fd  = -1;
    child->go_write_fd = -1;
    child->ready_read_fd = -1;
    child->ready_write_fd = -1;
    child->netns_fd = - 1;

    child->name = strdup(name);
    child->cmd_path = strdup(cmd_path);
    child->exec_path = strdup(exec_path);
    child->exec_argv = exec_args_parse(exec_path, exec_args, &child->exec_argc);

    if (ip_addr) {
        child->ip_addr = strdup(ip_addr);
    }

    // security
    if (lau->sudo_user && lau->drop_sudo) {
        child->drop_sudo = 1;
        child->uid = lau->sudo_uid;
        child->gid = lau->sudo_uid;
    }
    child->drop_caps = lau->drop_caps;
    child->drop_privs = lau->drop_privs;
    child->use_seccomp = lau->use_seccomp;

    return child;
}

/*  signal handling code */
static void lau_handle_signal(int signo, siginfo_t *info, void *ucontext)
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

static int lau_setup_signals(void)
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

/*  cmd-line parser  */

static int set_str(char **dir, struct get_opt *opt, const char *val_str)
{
    if (*dir) free(*dir);
    //*dir = validate_dir(opt->name, val_str);
    *dir = strdup(val_str);
    if (!*dir) {
        return log_errno_rf("%s strdup failed", opt->name);
    }

    return 0;
}

static int set_int(int *ival, struct get_opt *opt, const char *val_str)
{
    int val = atoi(val_str);
    if (val < 0) {
        return log_error_rf("%s cannot be negative", opt->name);
    }
    *ival = val;

    return 0;
}

static int set_start_order(struct myl_lau *lau, struct get_opt *opt, const char *val_str)
{
    int start_order = atoi(val_str);

    if (start_order < 0 || start_order > 1) {
        return log_error_rf("%s Must be 0 or 1", opt->name);
    }

    lau->start_order = start_order == 1;
    lau->sync_all = lau->start_order ? lau_sync_inorder : lau_sync_parallel;

    return 0;
}

// cmd-line parser
int lau_parse_argv(struct myl_lau *lau, int argc, char *argv[])
{
    struct get_opt opts[] = {
        { "help",   "This help",  0, 'h' },
        { "log",    "debug mode", 0, 'l' },
        { "base-dir", "Path for all run-time state (default=cwd)", 1, 'b' },
        { "src-dir",  "Path where cmd binarys live (default=cwd)",  1, 's' },
        { "netns-dir","Network namespace dir",  1, 'n' },
        { "rootfs-dir",  "Folder to mount into container using OverlayFS", 1, 'r' },
        { "start-order", "Start order (0=parallel,1=sequential)",  1, 'o', GETOPT_DEFINT(lau->start_order)  },
        { "start-delay", "Start delay order in secs",  1, 'd', GETOPT_DEFINT(lau->start_delay) },
        { "drop-sudo",  "Drop sudo privilge", 0, 'u', GETOPT_DEFINT(lau->drop_sudo) },
        { "drop-caps",  "Drop capabilities",  0, 'c', GETOPT_DEFINT(lau->drop_caps) },
        { "drop-privs", "Disable SET_NO_NEW_PRIVS", 0, 'p', GETOPT_DEFINT(lau->drop_privs) },
        { "use-seccomp", "Use seccomp filters", 0, 'e', GETOPT_DEFINT(lau->use_seccomp) }
    };

    char *examples[] = {
        "startorder=1 startdelay=5"
    };

    // process cmd-line options
    struct getopt_parse parse;
    int rc = getopt_init(&parse, argc, argv, ARR_LEN(opts), opts);
    if (rc) return rc;
    while ((rc = getopt_next(&parse)) >= 0) {
        struct get_opt *opt = getopt_curopt(&parse);
        switch(rc) {
        case 'h': print_usage(argv[0], ARRAY(opts), ARRAY(examples)); return -1;
        case 'l': verbose = 1; break;
        case 'b': rc = set_str(&lau->base_dir, opt, getopt_str(&parse)); break;
        case 's': rc = set_str(&lau->src_dir, opt, getopt_str(&parse)); break;
        case 'n': rc = set_str(&lau->netns_dir, opt, getopt_str(&parse)); break;
        case 'r': rc = set_str(&lau->rootfs_dir, opt, getopt_str(&parse)); break;
        case 'o': rc = set_start_order(lau, opt, getopt_str(&parse)); break;
        case 'd': rc = set_int(&lau->start_delay, opt, getopt_str(&parse)); break;
        case 'u': lau->drop_sudo = atoi(getopt_str(&parse)) != 0; break;
        case 'c': lau->drop_caps = atoi(getopt_str(&parse)) != 0; break;
        case 'p': lau->drop_privs = atoi(getopt_str(&parse)) != 0; break;
        case 'e': lau->use_seccomp = atoi(getopt_str(&parse)) != 0; break;
        }
        if (rc < 0) break;
    }

    if (rc != GETOPT_EOF) return rc;

    // final checks
    if (!lau->run_dir) {
        lau->run_dir = gen_path(lau->base_dir, "run_dir");
        if (!lau->run_dir) return -1;
        lau->need_basedir = 1;
    }
    if (!lau->netns_dir) {
        lau->netns_dir = gen_path(lau->base_dir, "netns_dir");
        if (!lau->netns_dir) return -1;
        lau->need_basedir = 1;
    }
    if (!lau->store_dir) {
        lau->store_dir = gen_path(lau->base_dir, "store_dir");
        if (!lau->store_dir) return -1;
        lau->need_basedir = 1;
    }

    if (lau->rootfs_dir) {
        // remove trailing slash
        int len = strlen(lau->rootfs_dir);
        if (len && lau->rootfs_dir[len - 1] == '/') {
            lau->rootfs_dir[len - 1] = '\0';
        }
    }

    // all done
    return 0;
}

// set defaults
int lau_init(struct myl_lau *lau)
{
    // set defaults
    lau->max_config = MAX_CONFIG;
    lau->start_order = START_ORDER == 1 ? 1 : 0;
    lau->start_delay = START_DELAY;
    lau->sync_all = lau->start_order ? lau_sync_inorder : lau_sync_parallel;

    // security
    lau->pid = getpid();
    lau->drop_sudo = DROP_SUDO;
    lau->drop_caps = DROP_CAPS;
    lau->drop_privs = DROP_PRIVS;
    lau->use_seccomp = USE_SECCOMP;

    lau->cur_dir = getcwd(NULL, 0);
    if (!lau->cur_dir)  {
        return log_errno_rf("get_cwd failed");
    }

    // setup default dirs
    lau->base_dir = gen_path(lau->cur_dir, BASE_DIR);
    if (!lau->base_dir) {
        return log_error_rf("gen-path base_dir failed");
    }

    lau->src_dir = strdup(lau->cur_dir);
    if (!lau->src_dir) {
        return log_errno_rf("strdup cur_dir failed");
    }

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

static void child_cleanup(struct myl_child *child)
{
    if (child->run && child->pid > 0) {
        shutdown_pid(child->pid, 10000);
        child->run = 0;
    }

    // release all fds
    close_fd(&child->go_read_fd);
    close_fd(&child->go_write_fd);
    close_fd(&child->ready_read_fd);
    close_fd(&child->ready_write_fd);
    close_fd(&child->netns_fd);

    // release bind mount
    if (child->netns_mounted) {
        umount2(child->netns_path, MNT_DETACH);
        unlink(child->netns_path);
        child->netns_mounted = 0;
    }

    if (child->rootfs_mounted) {
        umount2(child->rootfs_path, MNT_DETACH);
        child->rootfs_mounted = 0;
    }

    if (child->rootfs_created) {
        rmdir(child->rootfs_path);
    }

    // release overlay mount
    if (child->overlay_mounted) {
        umount2(child->rootfs_path, MNT_DETACH);
        child->overlay_mounted = 0;
        if (child->cmd_mounted) {
            // XXX an rootfs unmout clears all mounts
            child->cmd_mounted = 0;
        }
    }

    // release cmd mount
    if (child->cmd_mounted) {
        umount2(child->dst_path, MNT_DETACH);
        child->cmd_mounted = 0;
    }

    // release stack memory
    if (child->stack) {
        munmap(child->stack, child->stack_size);
        child->stack = NULL;
    }

    // release name,exec_path,...
    if (child->name) free(child->name);
    if (child->cmd_path) free(child->cmd_path);
    if (child->exec_path) free(child->exec_path);
    if (child->exec_argc) {
        for (int i = 0; i < child->exec_argc; i++) {
            free(child->exec_argv[i]);
        }
        free(child->exec_argv);
    }
    if (child->ip_addr) free(child->ip_addr);

    if (child->store_dir) free(child->store_dir);
    if (child->rootfs_path) free(child->rootfs_path);
    if (child->netns_path) free(child->netns_path);
    if (child->dst_path) free(child->dst_path);

    if (child->lower_path) free(child->lower_path);
    if (child->upper_path) free(child->upper_path);
    if (child->work_path) free(child->work_path);

    // all done
}

void lau_destroy(struct myl_lau *lau)
{
    for (int i = 0; i < lau->num_config; i++) {
        child_cleanup(&lau->configs[i]);
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

    lau = calloc(1, sizeof(*lau));
    if (!lau) return log_errno_rn("malloc lau-state failed");

    // init
    lau->host_netns_fd = -1;

    return lau;
}

int main(int argc, char *argv[])
{
    int ec = -1;

    // create state
    struct myl_lau *lau = lau_create();
    if (!lau) fatal_error("Failed to create launcher state");

    if (lau_init(lau) != 0) goto done;
    if (lau_parse_argv(lau, argc, argv) != 0) goto done;
    if (lau_setup_signals() != 0) goto done;

    // add containers
    struct myl_child *db  = lau_add(lau, "db", "db/server", "/bin/server", NULL, "10.0.0.1");
    struct myl_child *cli = lau_add(lau, "client", "client/client", "/bin/client", "--hostname 10.0.0.1", "10.0.0.2");
    if (!db || !cli) goto done;

    // setup/run containers
    if (lau_setup(lau) != 0) goto done;
    if (lau_start_all(lau) != 0) goto done;

    // cable client and db together
    if (lau_link_veths(lau, cli, db) != 0) goto done;

    if (lau->sync_all(lau) != 0) goto done;
    if (lau_wait_pids(lau) != 0) goto done;

    // no errors
    ec = 0;

done:   
    if (lau) lau_destroy(lau);

    return ec;
}
