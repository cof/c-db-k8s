/* SPDX-License-Identifier: MIT | (c) 2026 [cof] */

/*
 * launcher child API
 * ---------------------
 * See lau_child.h for API description.
 *
 * API sections
 * ------------
 * create child
 * configure child
 * run child
 * helpers
 */
#include <errno.h>
#include <sched.h>

#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/syscall.h>

#include "util.h"
#include "log.h"
#include "ns_util.h"
#include "lau_child.h"

// create new child
struct lau_child *lau_child_create(void)
{
    struct lau_child *child;

    child = malloc(sizeof(*child));
    if (!child) return log_errno_rn("malloc failed for lau_child");

    memset(child, 0, sizeof(*child));

    // init all fds to -1
    child->go_read_fd  = -1;
    child->go_write_fd = -1;
    child->ready_read_fd = -1;
    child->ready_write_fd = -1;
    child->netns_fd = - 1;

    return child;
}

/* free child state
 * note we MUST clean up active mounts BEFORE we exit.
 * either the child process does it or the launcher process.
 */
void lau_child_free(struct lau_child *child)
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
        // overlay unmount clears all mounts
        child->cmd_mounted = 0;
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

    if (child->lowerdir) free(child->lowerdir);
    if (child->upperdir) free(child->upperdir);
    if (child->workdir) free(child->workdir);

    // all done
}

// load config into child
int lau_child_cfg_load(struct lau_child *child, struct lau_config *cfg)
{
    if (!cfg->name) return log_error_rf("Missing container name");
    if (!cfg->cmd_path) return log_error_rf("Missing cmd_name");
    if (!cfg->exec_path) return log_error_rf("Missing exec_path");

    child->name = strdup(cfg->name);
    child->cmd_path = strdup(cfg->cmd_path);
    child->exec_path = strdup(cfg->exec_path);
    child->exec_argv = exec_args_parse(cfg->exec_path, cfg->exec_args, &child->exec_argc);

    if (cfg->ip_addr) {
        child->ip_addr = strdup(cfg->ip_addr);
    }

    return 0;
}

// set child netns name
int lau_child_set_netns(struct lau_child *child, const char *name, const char *suffix)
{
    if (!suffix) suffix = "";
    return gen_str(child->netns_name, sizeof(child->netns_name), "%s%s", name, suffix);
}

// set child veth name
int lau_child_set_veth(struct lau_child *child, const char *name, const char *prefix)
{
    if (!prefix) prefix = "";
    return gen_str(child->veth_name, sizeof(child->veth_name), "%s%s", prefix, name);
}

/*
 * parent brings veth up inside child namespace
 * - rename veth to eth0
 * - add ip addr
 * - set lo up
 * - set eth0 up
 */
int lau_child_net_setup(struct lau_child *child)
{
    char tmp[128];
    struct sbuf buf = SBUF_INIT(tmp, sizeof(tmp));

    log_debug("lau setup-network (name=%s ipaddr=%s" , child->name, child->ip_addr);

    // TODO move this lot into ns_util.c
    if (run_cmd(&buf, 0, "nsenter -t %d -n ip link set %s name eth0", child->pid, child->veth_name)) return -1;
    if (run_cmd(&buf, 0, "nsenter -t %d -n ip addr add %s/24 dev eth0", child->pid, child->ip_addr)) return -1;
    if (run_cmd(&buf, 0, "nsenter -t %d -n ip link set lo up", child->pid)) return -1;
    if (run_cmd(&buf, 0, "nsenter -t %d -n ip link set eth0 up", child->pid)) return -1;

    child->need_network = 0;

    return 0;
}

// run - create pipe / stack / clone flags
int lau_child_prep(struct lau_child *child)
{
    int fds[2];

    // create go sync pipe
    if (pipe(fds) == -1) return log_errno_rf("create go-pipe for %s failed", child->name);
    child->go_read_fd = fds[0];
    child->go_write_fd = fds[1];

    // create ready sync pipe
    if (pipe(fds) == -1) return log_errno_rf("create ready_pipe for %s failed", child->name);
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
    if (child->need_network) child->clone_flags |= CLONE_NEWNET;

    return 0;
}

/* prerun - parent calls this to setup child before we call clone.
 * note parent MUST switch to the child netns before calling clone
 * for the child process to start inside its own netns.
 */
int lau_child_prerun(struct lau_child *child)
{
    int num_err = 0;

    if (child->netns_mounted && switch_child_netns(&child->netns_fd, child->name)) {
        // switch failed
        num_err++;
    }

    return num_err;
}

// run - clone child aka fork the parent process
int lau_child_run(struct lau_child *child)
{
    child->pid = clone(lau_child_start, child->stack + child->stack_size, child->clone_flags, child);

    if (child->pid == -1) {
        // failed ?
        return log_errno_rf("clone child %s '%s' failed", child->name, child->exec_path);
    }

    child->run = 1;

    return 0;
}

/* postrun - release child state after we clone child process.
 * note child process inherits all memory and file descriptors
 * from the parent so we release here what we no longer need.
 */
int lau_child_postrun(struct lau_child *child, int netns_fd)
{
    int num_err = 0;
    int rc;

    // release netns
    if (child->netns_mounted) {
        rc = restore_host_netns(netns_fd);
        if (rc) num_err++;
    }

    // release unused pipe ends
    rc = sync_pipe_close(&child->go_read_fd,
        &child->ready_write_fd,
        "lau", "post-run", child->name, child->pid
    );
    if (rc) num_err++;

    // release overlay
    if (child->overlay_mounted) {
        rc = unmount_overlay(child->rootfs_path, child->name);
        if (rc) num_err++;
        child->overlay_mounted = 0;
        // overlay unmount clears all mounts
        child->cmd_mounted = 0;
    }

    // release cmd mount
    if (child->cmd_mounted) {
        rc = umount2(child->dst_path, MNT_DETACH);
        if (rc) num_err++;
        child->cmd_mounted = 0;
    }

    // release stack
    if (child->stack && !(child->clone_flags & CLONE_VM)) {
        rc = munmap(child->stack, child->stack_size);
        if (rc) num_err++;
        child->stack = NULL;
    }

    return num_err;
}

/* child process code */

// child process - set security
static int setup_priv(struct lau_child *child)
{
    log_debug("Container (name=%s pid=%d) setup-priv (uid=%d,gid=%d)",
        child->name, child->pid, child->uid, child->gid);

    if (child->drop_caps && drop_bounding_set(child->name)) return -1;
    if (child->drop_sudo && drop_sudo(child->name, child->uid , child->gid)) return -1;
    if (child->drop_caps && clear_all_caps(child->name)) return -1;
    if (child->drop_privs && drop_new_privs(child->name))  return -1;
    if (child->use_seccomp && apply_seccomp(child->name)) return -1;

    return 0;
}

// child process - send ready signal to parent
static int child_send_ready(struct lau_child *child)
{
    return sync_pipe_write(
        &child->ready_write_fd, child->sig,
        "Container", "send-ready", child->name, child->pid
    );
}

// child process - wait for go signal from parent
static int child_wait_go(struct lau_child *child)
{
    // close the pipe ends we don't need
    int rc = sync_pipe_close(&child->ready_read_fd, &child->go_write_fd,
        "Container", "wait-go", child->name, child->pid
    );
    if (rc) return rc;

    rc = sync_pipe_read(&child->go_read_fd, child->sig,
        "Container", "wait-go", child->name, child->pid
    );

    return rc;
}

// child process - starts here
int lau_child_start(void *arg)
{
    struct lau_child *child = arg;

    child->pid = getpid();
    log_debug("Container (name=%s pid=%d) started", child->name, child->pid);

    if (set_identity(child->name) != 0) _exit(1);
    if (child_wait_go(child) != 0) _exit(2);
    if (set_rootfs(child_get_rootfs(child)) !=0) _exit(3);
    if (set_proc() != 0) _exit(4);
    if (child->need_network && create_network(child->veth_name, child->ip_addr) != 0) _exit(5);
    if (child_send_ready(child) != 0) _exit(6);

    // close all remaining fds other than stdio,stdout,stderr
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

