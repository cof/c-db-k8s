/*
 * launcher : runtime container launcher
 * Usage    : ./launcher --help
 * Example  : sudo ./launcher
 *
 * Overview
 * --------
 * Implements a container launcher for running client and server.
 *
 * Bascially:
 * - Create a folder for each container to hold its rootfs
 * - create a veth device for client and db container
 * - create a network namespace (netns) for each container
 * - create a child process for each container
 * - child switches to its private rootfs
 * - child creates proc
 * - child applys security settings
 * - child execs the client or server binary
 *
 * Note:
 * - uses bind  mount to create netns (not ip netns add)
 * - uses clone to create container child process
 * - uses pipes for parent/child process sync
 */
#include <sys/wait.h>

#include "config.h"
#include "util.h"
#include "log.h"
#include "ns_util.h"
#include "lau_child.h"

// defaults
#define BASE_DIR "mylauncher"
#define MAX_PROC 20
#define START_ORDER 1
#define START_DELAY 1

// security
#define DROP_SUDO  1
#define DROP_CAPS  1
#define DROP_PRIVS  1
#define USE_SECCOMP 1

// launcher state
struct lau_ctx {
    char *cur_dir;      // cwd where laucher start
    char *base_dir;     // root dir for all launcher state
    char *src_dir;      // location of host cmd files live
    char *run_dir;      // location of launcher pid run file
    char *netns_dir;    // netns mount point overide e.g /var/run/netns
    char *store_dir;    // path where container dirs are createed
    char *rootfs_dir;   // path where a rootfs_dir lives
    char *netns_suffix; // suffix to add to nens name e.g name-ns
    char *veth_prefix;  // prefix to prepend to veth name e.g. veth-name
    int start_delay;    // delay in secs between starting each container
    int (*sync_all)(struct lau_ctx *lau);
    struct simple_sig sig; // signal handler state
    // container child process
    int max_proc;
    int num_proc;
    struct lau_child *procs[MAX_PROC];
    unsigned int num_run; // take a wild guess reader
    int netns_fd;      // default host netns
    mode_t dir_mode;   // mode for createing dirs
    pid_t pid;         // our pid
    // security
    char *sudo_user; // SUDO_USER value
    int sudo_uid;    // SUDO_UID value
    int sudo_gid;    // SUDO_GID value
    int euid;        // effective uid
    // flags - bit fields
    unsigned int need_basedir : 1; // create base dir
    unsigned int start_order  : 1; // start container in order
    unsigned int sudo_active  : 1; // launcher is run with sudo
    unsigned int drop_sudo    : 1; // drop sudo on containers
    unsigned int drop_caps    : 1; // drop capabilities
    unsigned int drop_privs   : 1; // prctl PR_SET_NO_NEW_PRIVS
    unsigned int use_seccomp  : 1; // use seccomp filters
    unsigned int use_name_id  : 1; // ???
    unsigned int use_subdirs  : 1; // rootfs
    unsigned int use_overlay  : 1; // lower,upper,work,merged
    unsigned int mount_cmds   : 1; // mount cmd files instead of copying
    unsigned int child_add_ip : 1; // child process sets up network
};

/* wait pids code */

// check if child mut be reaped - if exitted or recv a signal
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

    return log_error_rc(rc, "Container '%s' reaped (pid=%d why=%s)", child->name, child->pid, why);
}

// find child with matching pid
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

// wait for intr or child to be reaped
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

    // success only if child exit 0
    return status == LAU_EXIT_OK ? 0 : -1;
}

/* sync code */

// parent - send go signal to child
static int lau_child_send_go(struct lau_ctx *lau, struct lau_child *child)
{
    return sync_pipe_write(
        &child->go_write_fd, &lau->sig,
        "lau", "send-go", child->name, child->pid
    );
}

// parent - wait for ready signal from child
static int lau_child_wait_ready(struct lau_ctx *lau, struct lau_child *child)
{
    return sync_pipe_read(
        &child->ready_read_fd, &lau->sig,
        "lau", "wait-ready", child->name, child->pid
    );
}

// sequential start - i.e. ensure DB server is up before client
static int lau_sync_inorder(struct lau_ctx *lau)
{
    log_debug("Launcher sync %d containers in order", lau->num_proc);

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
    log_debug("Launcher sync %d containers in parallel", lau->num_proc);

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

// start a child container - return error count
static int lau_start_child(struct lau_ctx *lau, struct lau_child *child)
{
    log_debug("Launcher starting %s", child->name);

    int num_err = 0;
    if (lau_child_prerun(child)) num_err++;
    if (!num_err && lau_child_run(child)) num_err++;
    if (lau_child_postrun(child, lau->netns_fd)) num_err++;

    if (child->run) lau->num_run++;

    return num_err;
}

// start all child containers - any errors will fail start
static int lau_start_all(struct lau_ctx *lau)
{
    log_debug("Starting %d containers", lau->num_proc);

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

// link two containers using a single veth
static int lau_link_veths(struct lau_ctx *lau, struct lau_child *x, struct lau_child *y)
{
    int rc;

    log_debug("Launcher create-cable (left=%s, right=%s)", x->name, y->name);

    // create veth for x and y
    if ((rc = lau_child_set_veth(x, x->name, lau->veth_prefix))) return rc;
    if ((rc = lau_child_set_veth(y, y->name, lau->veth_prefix))) return rc;
    if ((rc = veth_add(x->veth_name, y->veth_name))) return rc;

    // move veth x end into x netns
    if ((rc = veth_setns(x->veth_name, int_tostr(x->pid)))) {
        veth_del(x->veth_name);
        return rc;
    }

    // move veth y end into y netns
    if ((rc = veth_setns(y->veth_name, int_tostr(y->pid)))) {
        veth_del(y->veth_name);
        return rc;
    }

    // setup link (eth0,addr,up)
    if ((rc = lau_child_net_setup(x))) {
        veth_del(x->veth_name);
        return rc;
    }

    // setup link (eth0,addr,up)
    if ((rc = lau_child_net_setup(y))) {
        veth_del(y->veth_name);
        return rc;
    }

    log_info("+", "Created veth pair: %s <-> %s", x->veth_name, y->veth_name);

    return 0;
}

// check if client must add network when cloned
static int lau_check_net(struct lau_ctx *lau, struct lau_child *child)
{
    if (child->ip_addr && lau->child_add_ip) {
        // tell child process to add network
        child->need_network = 1;
    }

    return 0;
}

// create a netns for container - note we dont use "ip netns add"
static int lau_create_netns(struct lau_ctx *lau, struct lau_child *child)
{
    // generate name e.g "name-ns"
    int rc = lau_child_set_netns(child, child->name, lau->netns_suffix);
    if (rc) return rc;

    // generate path e,g "/var/run/netns/name-ns"
    child->netns_path = gen_path(lau->netns_dir, child->netns_name);
    if (!child->netns_path) return -1;

    // bind mount path
    child->netns_fd = mount_netns(child->netns_path);
    if (child->netns_fd == -1) return -1;
    child->netns_mounted = 1;

    log_info("+", "Created network namespace: %s", child->netns_name);

    return 0;
}

// load cmd file from host into container filesystem
static int lau_load_cmd(struct lau_ctx *lau, struct lau_child *child)
{
    log_debug("Launcher load-cmd (name=%s, cmd=%s dst=%s)",
        child->name, child->cmd_path,  child->exec_path);

    int rc = -1;

    // Get absolute cmd-path
    char *cmd_path = child->cmd_path;
    if (!cmd_path) return log_error_rf("copy-cmd %s missing cmd_path", child->name);
    if (*cmd_path != '/') {
        cmd_path = gen_path(lau->src_dir, child->cmd_path);
        if (!cmd_path) return rc;
    }

    // Build dst-path - storedir/child/rootfs/exec_path
    char *dst_path = gen_path(child_get_rootfs(child), child->exec_path);
    if (!dst_path) goto done;
    rc = create_path_for_file(dst_path, lau->dir_mode);
    if (rc) goto done;

    if (lau->mount_cmds) {
        // mount the cmd into container store-dir
        rc = mount_cmd(cmd_path, dst_path);
        if (rc == 0) {
            child->cmd_mounted = 1;
            // need to store mount point
            child->dst_path = dst_path;
            dst_path = NULL;
        }
    }
    else {
        // copy file into child store-dir
        rc = copy_file(cmd_path, dst_path);
    }

done:
    if (cmd_path && cmd_path != child->cmd_path) free(cmd_path);
    if (dst_path) free(dst_path);

    return rc;
}

// mount overlayFS for child
static int lau_mount_overlay(struct lau_ctx *lau, struct lau_child *child)
{
    if (!lau->use_overlay) return 0;

    log_debug("lau add-overlay (name=%s)", child->name);

    int rc = mount_overlay(child->rootfs_path,
        child->lowerdir ?: lau->rootfs_dir,
        child->upperdir,
        child->workdir,
        child->name
    );
    if (rc) return rc;

    child->overlay_mounted = 1;

    return 0;
}

// FIXME - overlay works but not this
static int lau_mount_rootfs(struct lau_ctx *lau, struct lau_child *child)
{
    if (!lau->use_subdirs) return 0;
    if (!lau->rootfs_dir) return 0;
    if (lau->use_overlay) return 0;

    int rc = mount_rootfs(lau->rootfs_dir, child->rootfs_path);
    if (rc) return rc;

    child->rootfs_mounted = 1;

    log_debug("lau mount-rootfs (name=%s rootfs=%s)", child->name, child->rootfs_path);

    return 0;
}

// create subdirs in child store-dir
static int lau_create_subdirs(struct lau_ctx *lau, struct lau_child *child)
{
    if (!lau->use_subdirs) return 0;

    // create store-dir/rootfs
    child->rootfs_path = create_subdir(child->store_dir, "rootfs", lau->dir_mode);
    if (!child->rootfs_path) return -1;
    child->rootfs_created = 1;
    child->use_subdir = 1;

    // add overlay
    if (!lau->use_overlay) return 0;

    if (!lau->rootfs_dir) {
        // need store-dir/child-name/lower
        child->lowerdir = create_subdir(child->store_dir, "lower", lau->dir_mode);
        if (!child->lowerdir) return -1;
    }

    // store-dir/child-name/upper
    child->upperdir = create_subdir(child->store_dir, "upper", lau->dir_mode);
    if (!child->upperdir) return -1;

    // store-dir/child-name/work
    child->workdir = create_subdir(child->store_dir, "work", lau->dir_mode);
    if (!child->workdir) return -1;

    return 0;
}

// create a store-dir for container
static int lau_create_storedir(struct lau_ctx *lau, struct lau_child *child)
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
    int rc = create_dir(child->store_dir, lau->dir_mode, 1);
    if (rc) return rc;

    child->storedir_created = 1;

    log_info("lau create-storedir (name=%s storedir=%s)", child->name, child->store_dir);

    return 0;
}

// check if we use use store-dir or store-dir/rootfs dir for child
static int lau_check_rootfs(struct lau_ctx *lau)
{
    log_debug("Lau check-rootfs");

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

// open host netns file
static int lau_open_host_netns(struct lau_ctx *lau)
{
    int rc = open_host_netns();
    if (rc < 0) return rc;
    lau->netns_fd = rc;

    return 0;
}

// setup infrastucture
static int lau_setup_all(struct lau_ctx *lau)
{
    log_debug("Launcher setup infrastucture");

    RUN(lau_open_host_netns(lau));
   
    // create dirs
    if (lau->need_basedir) {
        RUN(create_path(lau->base_dir, lau->dir_mode));
    }
    RUN(create_dir(lau->netns_dir, lau->dir_mode, 1));
    RUN(create_dir(lau->store_dir, lau->dir_mode, 1));
    RUN(create_dir(lau->run_dir,   lau->dir_mode, 1));

    RUN(lau_check_rootfs(lau));

    for (int i = 0; i < lau->num_proc; i++) {
        struct lau_child *cnt = lau->procs[i];
        RUN(lau_create_storedir(lau, cnt));
        RUN(lau_create_subdirs(lau, cnt));
        RUN(lau_mount_rootfs(lau, cnt));
        RUN(lau_mount_overlay(lau, cnt));
        RUN(lau_load_cmd(lau, cnt));
        RUN(lau_check_net(lau, cnt));
        RUN(lau_create_netns(lau, cnt));
    }

    // all done
    return 0;
}

// create a new child for cfg
static struct lau_child *lau_add_child(struct lau_ctx *lau, struct lau_config *cfg)
{
    if (lau->num_proc >= lau->max_proc) {
        return log_error_rn("Add failed. num-proc %d >= max %d", lau->num_proc, lau->max_proc);
    }

    struct lau_child *child = lau_child_create();
    if (!child) return NULL;

    int rc = lau_child_cfg_load(child, cfg);
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

    log_debug("lau added child %d %s", lau->num_proc, child->name);

    return child;
}

// apply cmd-line settings and final checks
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

    // set sync order
    lau->sync_all = lau->start_order
        ? lau_sync_inorder
        : lau_sync_parallel;

    // all done
    return 0;
}

/* cmd-line */
// TODO use xmacros
enum {
    opt_help,
    opt_loglevel,
    opt_base_dir,
    opt_src_dir,
    opt_netns_dir,
    opt_rootfs_dir,
    opt_start_order,
    opt_start_delay,
    opt_drop_sudo,
    opt_drop_caps,
    opt_drop_privs,
    opt_use_seccomp
};

static struct cmd_opt opts[] = {
    // name, desc, def, has_arg
    { "--help", "This help", 0, 0 },
    { "--log-level", "logging level", STR(APP_LOGLEVEL), 1  },
    { "--base-dir",   "Path for all run-time state", "cwd", 1 },
    { "--src-dir",    "Path where cmd binarys live", "cwd", 1 },
    { "--netns-dir",  "Network namespace dir", 0,  1 },
    { "--rootfs-dir", "Folder to mount into container using OverlayFS", 0, 1 },
    { "--start-order", "Start order (0=sequential,1=parallel)", STR(START_ORDER), 1 },
    { "--start-delay", "Start delay order in secs", STR(START_DELAY), 1 },
    { "--drop-sudo",   "Drop sudo privilge",    STR(DROP_SUDO),   1 },
    { "--drop-caps",   "Drop capabilities",     STR(DROP_CAPS),   1 },
    { "--drop-privs",  "Dont SET_NO_NEW_PRIVS", STR(DROP_PRIVS),  1 },
    { "--use-seccomp", "Use seccomp filters",   STR(USE_SECCOMP), 1 },
    { NULL }
};

static const char *examples[] = {
    "--startorder 1 --startdelay 5",
    "--base-dir /home/alpine/test-lau --srcdir /home/alpine/bin",
    NULL
};

static int lau_parse_argv(struct lau_ctx *lau, int argc, char *argv[])
{
    struct cmd_argv parser = { argc, argv, opts };
    int rc;

    while ( (rc = cmd_argv_next(&parser)) >= 0) {
        switch(rc) {
        case opt_help: prog_usage(argv[0], opts, examples); exit(0);
        case opt_loglevel:    rc = opt_setint(&log_level, &parser); break;
        case opt_base_dir:    rc = opt_setstr(&lau->base_dir, &parser); break;
        case opt_src_dir:     rc = opt_setstr(&lau->src_dir, &parser); break;
        case opt_netns_dir:   rc = opt_setstr(&lau->netns_dir, &parser); break;
        case opt_rootfs_dir:  rc = opt_setstr(&lau->rootfs_dir, &parser); break;
        case opt_start_order: lau->start_order = atoi(parser.value) == 1; break;
        case opt_start_delay: rc = opt_setint(&lau->start_delay, &parser); break;
        case opt_drop_sudo:   lau->drop_sudo = atoi(parser.value) == 1; break;
        case opt_drop_caps:   lau->drop_caps = atoi(parser.value) == 1; break;
        case opt_drop_privs:  lau->drop_privs = atoi(parser.value) == 1; break;
        case opt_use_seccomp: lau->use_seccomp = atoi(parser.value) == 1; break;
        }
        if (rc < 0) break;
    }

    return rc == OPT_EOF ? 0 : -1;
}

// set defaults
int lau_init(struct lau_ctx *lau)
{
    log_level = LOG_ERROR;

    lau->max_proc = ARR_LEN(lau->procs);
    lau->start_order = START_ORDER == 1 ? 1 : 0;
    lau->start_delay = START_DELAY;

    // security
    lau->pid = getpid();
    lau->drop_sudo = DROP_SUDO;
    lau->drop_caps = DROP_CAPS;
    lau->drop_privs = DROP_PRIVS;
    lau->use_seccomp = USE_SECCOMP;

    lau->cur_dir = getcwd(NULL, 0);
    if (!lau->cur_dir) return log_errno_rf("get_cwd failed");

    // setup default dirs
    lau->base_dir = gen_path(lau->cur_dir, BASE_DIR);
    if (!lau->base_dir) return -1;
    str_setval(&lau->src_dir, "cur-dir", lau->cur_dir);
    if (!lau->src_dir) return -1;
    str_setval(&lau->netns_suffix, "netns-suffix", "-ns");
    if (!&lau->netns_suffix) return -1;
    str_setval(&lau->veth_prefix, "veth-prefix", "veth-");
    if (!&lau->veth_prefix) return -1;

    lau->dir_mode = STANDARD_MODE;
    lau->euid = geteuid();

    // get sudo
    char *env;
    if ((env = getenv("SUDO_USER")) != NULL) {
        str_setval(&lau->sudo_user, "sudo_user", env);
        if (!lau->sudo_user) return -1;
    }
    if ((env = getenv("SUDO_UID")) != NULL) {
        int_setval(&lau->sudo_uid, "sudo_uid", env);
        if (!lau->sudo_uid) return -1;
    }
    if ((env = getenv("SUDO_GID")) != NULL) {
        int_setval(&lau->sudo_gid, "sudo_gid", env);
        if (!lau->sudo_gid) return -1;
    }

    return 0;
}

// reap children - free memory
static void lau_destroy(struct lau_ctx *lau)
{
    for (int i = 0; i < lau->num_proc; i++) {
        lau_child_free(lau->procs[i]);
    }
    lau->num_proc = 0;

    close_fd(&lau->netns_fd);

    if (lau->cur_dir) free(lau->cur_dir);
    if (lau->src_dir) free(lau->src_dir);

    if (lau->netns_dir) free(lau->netns_dir);
    if (lau->store_dir) free(lau->store_dir);
    if (lau->rootfs_dir) free(lau->rootfs_dir);

    if (lau->netns_suffix) free(lau->netns_suffix);
    if (lau->veth_prefix) free(lau->veth_prefix);

    free(lau);
}

// create launcher state
static struct lau_ctx *lau_create(void)
{
    struct lau_ctx *lau;

    lau = malloc(sizeof(*lau));
    if (!lau) return log_errno_rn("malloc lau-state failed");
    memset(lau, 0, sizeof(*lau));

    // init all fds to -1
    lau->netns_fd = -1;

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

    log_init(NULL, LOG_INFO);

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
