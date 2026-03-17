/* 
 * launcher -a runtime container launcher
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
    pid_t pid;
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


static inline int child_reaped(int status)
{
    return WIFEXITED(status) | WIFSIGNALED(status) ? 1 : 0;
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
        log_info("LOG", "Container (name=%s pid=%d) recv-go", cnt->name, cnt->child_pid);
    }

    return 0;;
}

static int child_wake_sync(struct myl_cnt *cnt)
{
    if (verbose)  {
        log_info("LOG", "Container (name=%s pid=%d) send-ready", cnt->name, cnt->child_pid);
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


static inline char *get_rootfs(struct myl_cnt *cnt)
{
    return cnt->use_subdir ? cnt->rootfs_path : cnt->root_path;
}

static int setup_priv(struct myl_cnt *cnt)
{
    if (verbose) {
        log_info("LOG", "Container (name=%s pid=%d) setup-priv (uid=%d,gid=%d)", 
            cnt->name, cnt->child_pid, cnt->uid, cnt->gid);
    }

    if (cnt->drop_caps && drop_bounding_set(cnt->name)) return -1;
    if (cnt->drop_sudo && drop_sudo(cnt->name, cnt->uid , cnt->gid)) return -1;
    if (cnt->drop_caps && clear_all_caps(cnt->name)) return -1;
    if (cnt->drop_privs && drop_new_privs(cnt->name))  return -1;
    if (cnt->use_seccomp && apply_seccomp(cnt->name)) return -1;

    return 0; 
}

// child process starts here
static int lau_cnt_start(void *arg)
{
    struct myl_cnt *cnt = arg;

    cnt->child_pid = getpid();
    if (verbose) {
        log_info("LOG", "Container (name=%s pid=%d) started", cnt->name, cnt->child_pid);
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
    if (verbose) {
        log_info("LOG", "Launcher starting %s", cnt->name);
    }

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

    log_info("+", "Created network namespace: %s", cnt->netns_name);

    return 0;
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
        log_info("LOG", "Launcher copy-files (name=%s, cmd=%s)", cnt->name, cnt->cmd_path);
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
        log_info("LOG", "launcher setup-network (name=%s ipaddr=%s" , cnt->name, cnt->ip_addr);
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
            log_info("+", "Container '%s' exit ok (pid=%d why=%s)", cnt->name, cnt->child_pid, why);
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
        log_info("LOG", "Launcher (name=%s pid=%d) send-go", cnt->name, cnt->child_pid);
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
        log_info("LOG", "Launcher (name=%s pid=%d) recv-ready", cnt->name, cnt->child_pid);
    }

    return 0;
}

int lau_sync(struct myl_lau *lau)
{
    if (verbose) {
        log_info("LOG", "Launcher sync %d containers %s", 
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
        log_info("LOG", "Launcher create-cable (left=%s, right=%s)", x->name, y->name);
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

    log_info("+", "Created veth pair: %s <-> %s", x->veth_name, y->veth_name);

    return 0;
}

int lau_run(struct myl_lau *lau)
{
    if (verbose) {
        log_info("LOG", "Launcher starting %d containers", lau->num_config);
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
        log_info("LOG", "Launcher setup infrastucture");
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

static int set_dir(char **dir, struct get_opt *opt, const char *val_str)
{
    if (*dir) free(*dir);
    *dir = validate_dir(opt->name, val_str);
    if (!*dir) return -1;

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
            log_info("LOG", "waitpid reaped unknown child pid %d", pid);
            continue;
        }
        // check if running 
        cnt->status = status;
        status = lau_check_reaped(lau, cnt);
        if (status != 0) {
            break;
        }
    }

    if (caught_signo) {
        log_info("+","PID:%d shutting down: got signal %d (%s) from UID:%d PID:%d ", 
            lau->pid, 
            caught_signo, strsignal(caught_signo), 
            sender_uid,
            sender_pid);
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

int lau_parse_argv(struct myl_lau *lau, int argc, char *argv[])
{
    struct get_opt opts[] = {
        { "help",   "This help", 0, 'h' },
        { "verbose","debug verbose mode", 0, 'v' },
        { "srcdir",  "Path where containers binarys live (default=cwd)",  1, 's' },
        { "netnsdir", "Network namespace dir",  1, 'n' },
        { "rootfs",  "rootfs dir to mount into container using OverlayFS", 1, 'r' },
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
        case 'v': verbose = 1; break;
        case 's': rc = set_dir(&lau->src_dir, opt, getopt_str(&parse)); break;
        case 'n': rc = set_dir(&lau->netns_dir, opt, getopt_str(&parse)); break;
        case 'r': rc = set_dir(&lau->rootfs_dir, opt, getopt_str(&parse)); break;
        case 'o': lau->start_order = atoi(getopt_str(&parse)) != 0; break;
        case 'd': rc = set_int(&lau->start_delay, opt, getopt_str(&parse)); break;
        case 'u': lau->drop_sudo = atoi(getopt_str(&parse)) != 0; break;
        case 'c': lau->drop_caps = atoi(getopt_str(&parse)) != 0; break;
        case 'p': lau->drop_privs = atoi(getopt_str(&parse)) != 0; break;
        case 'e': lau->use_seccomp = atoi(getopt_str(&parse)) != 0; break;
        }
        if (rc < 0) break;
    }

    return rc == GETOPT_EOF ? 0 : -1;
}

int lau_init(struct myl_lau *lau)
{
    // set defaults
    lau->max_config = MAX_CONFIG;
    lau->start_delay = START_DELAY;
    lau->start_order = START_ORDER;

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
        return log_errno_rf("gen_path mylaucher");
    }
    lau->run_dir   = gen_path(lau->base_dir, "run_dir");
    lau->netns_dir = gen_path(lau->base_dir, "netns_dir");
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
    struct myl_cnt *cli = lau_add(lau, "client", "client/client", "/bin/client", "--hostname 10.0.0.1", "10.0.0.2");
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
