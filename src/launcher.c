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
#include "lau_child.h"

// config defaults
#define BASE_DIR "mylauncher"
#define RUN_DIR "/run/asimple_launcher"
#define STORE_dir "/var/lib/asimple_launcher"

#define MAX_CHILD 10
#define START_ORDER 1
#define START_DELAY 1

// security
#define DROP_SUDO  1
#define DROP_CAPS  1
#define DROP_PRIVS  1
#define USE_SECCOMP 1

// launcher state
struct lau_ctx {
    const char *prog_name; // argv[0]
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
    int (*sync_all)(struct lau_ctx *lau);
    struct simple_sig sig;
    // container child process
    int max_proc;
    int num_proc;
    struct lau_child *procs[MAX_CHILD];
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


/* wait pids code */
static int lau_check_reaped(struct lau_ctx *lau, struct lau_child *child, int status)
{
    child->status = status;

    if (!is_reaped(child->status)) return 0;

    child->run = 0;
    lau->num_run--;

    // check reason why child was reaped
    int rc = LAU_ERR;
    char why[40];
    if (WIFEXITED(child->status)) {
        // child exited
        int exit_code = WEXITSTATUS(child->status);
        snprintf(why, sizeof(why), "exit %d", exit_code);
        if (exit_code == 0) {
            // child exited okay
            log_info("+", "Container '%s' exit ok (pid=%d why=%s)", child->name, child->pid, why);
            return LAU_EXIT_OK;
        }
    }
    else if (WIFSIGNALED(child->status)) {
        // child got signal
        int sig = WTERMSIG(child->status);
        snprintf(why, sizeof(why), "signal %d (%s)", sig, strsignal(sig));
    }
    else {
        // unknown reason
        snprintf(why, sizeof(why), "unknown 0x%08x", child->status);
    }

    return log_error_re(rc, "Container '%s' reaped (pid=%d why=%s)", child->name, child->pid, why);
}

static struct lau_child *lau_find_child(struct lau_ctx *lau, pid_t pid)
{
    for (int i = 0; i < lau->num_proc; i++) {
        struct lau_child *child = lau->procs[i];
        if (child->pid == pid) {
            return child;
        }
    }

    // not found
    return NULL;
}

// wait for intr or a child exit
static int lau_wait_pids(struct lau_ctx *lau)
{
    int status = 0;

    while (lau->sig.run && lau->num_run > 0) {
        // wait for child status
        pid_t pid = waitpid(-1, &status, 0); 
        if (pid == 0) continue;
        // check if waitpid failed
        if (pid == -1) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) {
                // no more children - stop now
                for (int i = 0; i < lau->num_proc; i++) {
                    lau->procs[i]->run = 0;
                }
                lau->num_run = 0;
                break;
            }
            return log_errno_rf("waitpid failed");;
        }
        // update child status
        struct lau_child *child = lau_find_child(lau, pid);
        if (!child) {
            log_info("LOG", "waitpid reaped unknown pid %d", pid);
            continue;
        }
        // check if child stll running 
        status = lau_check_reaped(lau, child, status);
        if (status != 0) break;
    }

    // success only if allchild exit 0
    return status == LAU_EXIT_OK ? 0 : -1;
}

/* sync all code */

static int lau_child_send_go(struct lau_ctx *lau, struct lau_child *child)
{
    return sync_wrpipe(
        &child->go_write_fd, &lau->sig, 
        "lau", "send-go", child->name, child->pid
    );
}

static int lau_child_wait_ready(struct lau_ctx *lau, struct lau_child *child)
{
    return sync_rdpipe(
        &child->ready_read_fd, &lau->sig,
        "lau", "wait-ready", child->name, child->pid
    );
}

// sequential start - i.e. ensure DB server is up before client
static int lau_sync_inorder(struct lau_ctx *lau) 
{
    if (verbose) {
        log_info("LOG", "Launcher sync %d containers in order", lau->num_proc);
    }

    for (int i = 0; i < lau->num_proc; i++) {
        struct lau_child *child = lau->procs[i];
        int rc = lau_child_send_go(lau, child);
        if (rc) return rc;
        rc = lau_child_wait_ready(lau, child);
        if (rc) return rc;
        sleep(lau->start_delay);
    }

    return 0;
}

// parallel start - note clone/fork start order is undefined by OS
static int lau_sync_parallel(struct lau_ctx *lau) 
{
    if (verbose) {
        log_info("LOG", "Launcher sync %d containers in parallel", lau->num_proc);
    }

    for (int i = 0; i < lau->num_proc; i++) {
        int rc = lau_child_send_go(lau, lau->procs[i]);
        if (rc) return rc;
    }

    for (int i = 0; i < lau->num_proc; i++) {
        int rc = lau_child_wait_ready(lau, lau->procs[i]);
        if (rc) return rc;
    }

    return 0;
}

static int lau_restore_netns(struct lau_ctx *lau)
{
    int rc = setns(lau->host_netns_fd, CLONE_NEWNET);
    if (rc) {
        log_errno_rf("restore netns %s failed", HOST_NETNS_PATH);
    }

    return 0;
}

int lau_child_prerun(struct lau_ctx *lau, struct lau_child *child)
{
    (void) lau;

    int num_err = 0;

    if (child->netns_mounted && lau_child_switch_netns(child)) {
        num_err++;
    }

    return 0;
}

int lau_child_postrun(struct lau_ctx *lau, struct lau_child *child)
{
    int num_err = 0;
    int rc;

    if (child->netns_mounted) {
        rc = lau_restore_netns(lau);
        if (rc) num_err++;
    }

    rc = sync_rdwr_close(&child->go_read_fd, &child->ready_write_fd,
        "lau", "post-run", child->name, child->pid
    );
    if (rc) num_err++;

    // release overlay mount
    if (child->overlay_mounted) {
        rc = umount2(child->rootfs_path, MNT_DETACH);
        if (rc < 0 && errno != EINVAL) {
            log_errno("unmount overlay %s for %s failed", child->rootfs_path, child->name);
            num_err++;
        }
        child->overlay_mounted = 0;
    }

    return num_err;
}

static int lau_start_child(struct lau_ctx *lau, struct lau_child *child)
{
    if (verbose) {
        log_info("LOG", "Launcher starting %s", child->name);
    }

    int num_err = 0;

    if (lau_child_prerun(lau, child)) num_err++;
    if (!num_err && lau_child_run(child)) num_err++;
    if (lau_child_postrun(lau, child)) num_err++;

    if (child->run) lau->num_run++;

    // done
    return num_err;
}

static int lau_start_all(struct lau_ctx *lau)
{
    if (verbose) {
        log_info("LOG", "Starting %d containers", lau->num_proc);
    }

    for (int i = 0; i < lau->num_proc; i++) {
        int rc = lau_child_prep(lau->procs[i]);
        if (rc) return rc;
    }

    for (int i = 0; i < lau->num_proc; i++) {
        int rc = lau_start_child(lau, lau->procs[i]);
        if (rc) return rc;
    }

    return 0;
}


static int set_cable_name(struct lau_ctx *lau, struct lau_child *child)
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

static int lau_link_veths(struct lau_ctx *lau, struct lau_child *x, struct lau_child *y)
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

    RUN(lau_child_setup_network(x));
    RUN(lau_child_setup_network(y));

    log_info("+", "Created veth pair: %s <-> %s", x->veth_name, y->veth_name);

    return 0;
}

static int check_network(struct lau_ctx *lau, struct lau_child *child)
{
    if (child->ip_addr && lau->child_add_ip) { 
        child->need_network = 1;
    }

    return 0;
}

static int create_netns(struct lau_ctx *lau, struct lau_child *child)
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
static int load_cmd(struct lau_ctx *lau, struct lau_child *child)
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

static int mount_overlay(struct lau_ctx *lau, struct lau_child *child)
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

static int mount_rootfs(struct lau_ctx *lau, struct lau_child *child)
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

static int create_subdirs(struct lau_ctx *lau, struct lau_child *child)
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
static int create_storedir(struct lau_ctx *lau, struct lau_child *child)
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


static int check_rootfs_dir(struct lau_ctx *lau)
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
static int lau_open_host_netns(struct lau_ctx *lau)
{
    // fd must be for this process only (O_CLOEXEC)
    lau->host_netns_fd = open(HOST_NETNS_PATH, O_RDONLY | O_CLOEXEC);
    if (lau->host_netns_fd == -1) {
        return log_errno_rf("open host_netns %s failed", HOST_NETNS_PATH);
    }

    return 0;
}

// setup infrastucture
static int lau_setup_all(struct lau_ctx *lau)
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

    for (int i = 0; i < lau->num_proc; i++) {
        struct lau_child *cnt = lau->procs[i];
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

static struct lau_child *lau_add_child(struct lau_ctx *lau, struct lau_config *cfg)
{
    if (lau->num_proc >= lau->max_proc) { 
        return log_error_rn("Add failed. num-proc %d >= max %d", lau->num_proc, lau->max_proc);
    }

    struct lau_child *child = lau_child_create();
    if (!child) return NULL;

    int rc = lau_child_load_cfg(child, cfg);
    if (rc) {
        lau_child_free(child);
        return NULL;
    }

    // add security
    if (lau->sudo_user && lau->drop_sudo) {
        child->drop_sudo = 1;
        child->uid = lau->sudo_uid;
        child->gid = lau->sudo_uid;
    }
    child->drop_caps = lau->drop_caps;
    child->drop_privs = lau->drop_privs;
    child->use_seccomp = lau->use_seccomp;

    // add signal handler
    child->sig = &lau->sig;

    lau->procs[lau->num_proc++] = child;

    return child;
}

static int lau_apply_cfg(struct lau_ctx *lau)
{
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

    lau->sync_all = lau->start_order ? lau_sync_inorder : lau_sync_parallel;

    return 0;
}

/* cmd-line */
enum {
    SET_HELP = 0,
    SET_LOG,
    SET_START_ORDER,
    SET_DROP_SUDO,
    SET_DROP_CAPS,
    SET_DROP_PRIVS,
    SET_USE_SECCOMP
};
static int set_flag(void *arg, size_t flag, const char *name, const char *val);

static struct cmd_opt opts[] = {
    OPT_FLAG("--help", "This help", SET_HELP, set_flag), 
    OPT_FLAG("--log", "debug mode", SET_LOG,  set_flag),
    OPT_STR("--base-dir",   "Path for all run-time state", "cwd", struct lau_ctx, base_dir),
    OPT_STR("--src-dir",    "Path where cmd binarys live", "cwd", struct lau_ctx, src_dir),
    OPT_STR("--netns-dir",  "Network namespace dir", 0,  struct lau_ctx, base_dir),
    OPT_STR("--rootfs-dir", "Folder to mount into container using OverlayFS", 0, struct lau_ctx, rootfs_dir),
    OPT_BOOL("--start-order", "Start order (0=sequential,1=parallel)", STR(START_ORDER), SET_START_ORDER, set_flag),
    OPT_INT("--start-delay", "Start delay order in secs", STR(START_DELAY), struct lau_ctx, start_delay),
    OPT_BOOL("--drop-sudo",  "Drop sudo privilge", STR(DROP_SUDO), SET_DROP_SUDO, set_flag),
    OPT_BOOL("--drop-caps",  "Drop capabilities",  STR(DROP_CAPS), SET_DROP_CAPS, set_flag),
    OPT_BOOL("--drop-privs", "Dont SET_NO_NEW_PRIVS", STR(DROP_PRIVS),  SET_DROP_PRIVS, set_flag),
    OPT_BOOL("--use-seccomp", "Use seccomp filters",  STR(USE_SECCOMP), SET_USE_SECCOMP, set_flag),
    { NULL }
};

static const char *examples[] = {
    "startorder=1 startdelay=5",
    NULL
};

static int set_flag(void *arg, size_t flag, const char *name, const char *val)
{
    struct lau_ctx *lau = arg;
    (void) name;
    (void) val;

    switch(flag) {
    case SET_HELP: print_usage(lau->prog_name, opts, examples); exit(0);
    case SET_LOG: return verbose = 1, 0;
    case SET_START_ORDER: return lau->start_order = 1, 0;
    case SET_DROP_SUDO: return lau->drop_sudo = 1, 0;
    case SET_DROP_CAPS: return lau->drop_caps = 1, 0;
    case SET_DROP_PRIVS: return lau->drop_privs = 1, 0;
    case SET_USE_SECCOMP: return lau->use_seccomp = 1, 0;
    default: return -1;
    }
}

static int lau_parse_argv(struct lau_ctx *lau, int argc, char *argv[])
{
    lau->prog_name = argv[0];
    int rc = parse_argv(argc, argv, opts, lau);
    return rc >= 0 ? 0 : -1;
}

// set defaults
int lau_init(struct lau_ctx *lau)
{
    // set defaults
    lau->max_proc = MAX_CHILD;
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
    if (!lau->base_dir) return -1;

    str_setval(&lau->src_dir, "cur_dir", lau->cur_dir);
    if (!lau->src_dir) return -1;

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

static void lau_destroy(struct lau_ctx *lau)
{
    for (int i = 0; i < lau->num_proc; i++) {
        lau_child_free(lau->procs[i]);
    }
    lau->num_proc = 0;

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

static struct lau_ctx *lau_create(void)
{
    struct lau_ctx *lau;

    lau = malloc(sizeof(*lau));
    if (!lau) {
        return log_errno_rn("malloc lau-state failed");
    }

    memset(lau, 0, sizeof(*lau));

    // init all fds to -1
    lau->host_netns_fd = -1;

    return lau;
}

// define containers 
static struct lau_config db_cfg = {
    .name = "db", 
    .cmd_path = "db/server", 
    .exec_path = "/bin/server", 
    .ip_addr = "10.0.0.1"
};

static struct lau_config client_cfg = {
    .name = "client", 
    .cmd_path = "client/client", 
    .exec_path = "/bin/client", 
    .exec_args = "--hostname 10.0.0.1", 
    .ip_addr = "10.0.0.2"
};

int main(int argc, char *argv[])
{
    int ec = -1;

    // create state
    struct lau_ctx *lau = lau_create();
    if (!lau) fatal_error("Failed to create launcher state");

    if (lau_init(lau) != 0) goto done;
    if (lau_parse_argv(lau, argc, argv) != 0) goto done;
    if (lau_apply_cfg(lau) != 0) goto done;
    if (setup_signals(&lau->sig) != 0) goto done;

    // add containers
    struct lau_child *db  = lau_add_child(lau, &db_cfg);
    struct lau_child *cli = lau_add_child(lau, &client_cfg);
    if (!db || !cli) goto done;

    // setup/run containers
    if (lau_setup_all(lau) != 0) goto done;
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
