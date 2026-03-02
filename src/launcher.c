/* 
 * launcher -a simple container launcher
 * 
 *  CLONE_NEWUTS - private Hostname and NIS
 *  CLONE_PID    - private PID namespace
 *  CLONE_NEWNS  - privae mount namepsace
 *  CLONE_NEWNET - private network
 *
 * Notes
 * -
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
#include <unistd.h>
#include <wordexp.h>
#include <libgen.h>
#include <errno.h>
#include <sched.h>
#include <limits.h>
#include <time.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/syscall.h> 
#include <sys/sendfile.h>
#ifdef SECURITY
#include <sys/prctl.h>
#include <linux/capability.h>
#endif
#include <fcntl.h>
#include "util.h"

#define RUNTIME_DIR "/run/asimple_launcher"
#define STORAGE_DIR "/var/lib/asimple_launcher"
#define NETNS_DIR "/var/run/netns"
#define HOST_NETNS_PATH "/proc/self/ns/net"

#define MAX_CONFIG 10
#define START_ORDER 1
#define START_DELAY 2

static int verbose;

bool str_starts_with(const char *str, const char *prefix) {

    while (*prefix) {
        if (*prefix++ != *str++) return false;
    }

    return true;
}

char *gen_path(const char *dir, const char *name)
{
    if (!dir || !name) return NULL;

    char *path = NULL;
    int rc = asprintf(&path, "%s/%s", dir, name);

    if (rc == -1) {
        // out of memory ?
        return NULL;
    }

    return path;
}

static int inline child_reaped(int status)
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
        return log_errno("vsnprintf failed");
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
        return log_errno("create_dir %s failed", path);
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
        return log_errno("create_path strdup %s failed", path);

    int rc = create_path_nocopy(tmp, mode);
    free(tmp);

    return rc; 
}


static int touch_file(const char *path, int flags, mode_t mode)
{
    int fd = open(path, flags, mode);
    if (fd == -1) {
        return log_errno("open %s failed", path);
    }

    if (close(fd) != 0) {
        // close failed -> rm file and return 
        int _errno = errno;
        unlink(path);
        errno = _errno; 
        return log_errno("close %s failed", path);
    }

    return 0;
}

static int mount_file(const char *path)
{
    // make file a mount point
    if (mount(path, path, "none", MS_BIND, NULL) < 0) {
        return log_errno("mount self-bind %s failed", path);
    }

    // make file private
    if (mount("", path, NULL, MS_PRIVATE, NULL) < 0) {
        int _errno = errno;
        umount2(path, MNT_DETACH);
        errno = _errno;
        return log_errno("mount private %s failed", path);
    }

    return 0;
}

// bind mount a new nets
static int mount_netns(const char *netns_path)
{
    // create path 
    int netns_fd = touch_file(netns_path, O_RDONLY | O_CREAT | O_EXCL, 0600);
    if (netns_fd == -1) {
        return log_errno("open %s failed", netns_path);
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
        return log_errno("fork for bind mount failed");
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
        return log_errno("waitpid failed");
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        int _errno = errno;
        umount2(netns_path, MNT_DETACH);
        unlink(netns_path);
        errno = _errno;
        return log_error("failed to bind mount %s", netns_path);
    }

    // reopen netns 
    netns_fd = open(netns_path, O_RDONLY | O_CLOEXEC);
    if (netns_fd == -1) {
        int _errno = errno;
        umount2(netns_path, MNT_DETACH);
        unlink(netns_path);
        errno = _errno;
        return log_errno("reopen %s failed", netns_path);
    }

    if (umount2(netns_path, MNT_DETACH) < 0) {
        int _errno = errno;
        unlink(netns_path);
        errno = _errno;
        return log_errno("reopen %s failed", netns_path);
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
            return log_errno("snprintf: veth name failed");

        if (rc == 0 || rc >= veth_len)
            return log_error("snprintf: incorrct len %d", rc);

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
        return log_errno("sethostname %s failed", name);

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
        return log_errno("private mount failed");

    // - create new mount point
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) != 0)
        return log_errno("bind mount roofs %s failed", rootfs);

    // - change dir to new mount point
    if (chdir(rootfs) != 0)
        return log_errno("chdir rootfs %s", rootfs);

    // pivot_root to new mount point (stack old_root)
    if (syscall(SYS_pivot_root, ".", ".") == -1)
        return log_errno("pivot_root failed");

    // - change root dir to new mount  task_struct (root ptr)
    if (chroot(".") != 0)
        return log_errno("chroot failed");

    // - change curret dir to new root - task_struct (cwd ptr)
    if (chdir("/") != 0)
        return log_errno("chdir to / failed");

    if (umount2("/", MNT_DETACH) != 0)
        return log_errno("unmount2 / failed");

    // all done
    return 0;
}

static int set_proc(void)
{
    // new PID namespace - create new /proc
    if (create_dir("/proc", 0755, 1) != 0) {
        return log_errno("mkdir /proc failed");
    }

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        return log_errno("mount /proc faild");
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

struct container_config {
    char *name;          // container name
    char *cmd_path;  //  location of cmd
    char *exec_path;  // process to lanuch
    char **exec_argv; // command line args
    int exec_argc;
    char *ip_addr;    // ip addr to add to veth
    char *root_path;     // location of container dir
    char *rootfs_path;   // For the bind mount and pivot_root()
    char *netns_path;    // bind mounted network namespae
    char *dst_path;      // bind mounted dcmd path
    // used by overlay FS
    char *lower_path;   
    char *upper_path;   
    char *work_path;   
    char netns_name[IFNAMSIZ]; // network namespace name
    char veth_name[IFNAMSIZ];  // container eth0 link
    int netns_fd;
    // parent sync child
    int go_read_fd;    // child reads
    int go_write_fd;   // parent writes
    // child sync with parent
    int ready_read_fd; // parent reads
    int ready_write_fd; // child writes
    void *stack;       // passed to clone
    size_t stack_size;
    pid_t child_pid;   // value returned by clone
    int status; // waitpid
    unsigned int use_subdir : 1; // use a rootfs subdir instead of name
    unsigned int need_network : 1; // configure network
    unsigned int need_priv : 1; // prctl|drop_capabilities|apply_seccomp
    unsigned int run : 1; // clone child is active
    unsigned int netns_mounted : 1; // netns active
    unsigned int overlay_mounted : 1; // overlay FS active
    unsigned int cmd_mounted : 1; // cmd file was mounted
    // waitpid flags
    unsigned int exit : 1;
    unsigned int signalled : 1;
};

struct launcher_state {
    char *cur_dir; // cwd where laucher start
    char *src_dir; // where host files live
    char *netns_dir;  // netns mounts /var/run/netns
    char *runtime_dir; 
    char *storage_dir;
    char *rootfs_dir; // where a rootfs lives
    struct container_config configs[MAX_CONFIG];
    int host_netns_fd;
    int max_cfg;
    int num_cfg;
    int num_run;
    char *netns_suffix;
    char *cable_prefix;
    mode_t dir_mode;
    int start_delay;
    unsigned int start_order : 1; // start container in order
    unsigned int use_name_id : 1;
    unsigned int use_subdirs : 1; // rootfs
    unsigned int use_overlay : 1; // lower,upper,work,merged
    unsigned int use_cmd_mount : 1; // mount cmd files instead of copying 
    unsigned int child_add_ip : 1;  // child sets up network
};

static int child_wait_sync(struct container_config *cfg)
{
    // close the pipe ends we don't need
    if (close_fd(&cfg->go_write_fd) != 0) {
        return log_errno("close go_write for %s failed", cfg->name);
    }
    if (close_fd(&cfg->ready_read_fd) != 0) {
        return log_errno("close ready_read for %s failed", cfg->name);
    }

    // read 1 byte from parent
    ssize_t nr = read_sync(cfg->go_read_fd);
    if (nr == -1) {
        return log_errno("read-sync for %s failed", cfg->name);
    }
    if (nr != 1) {
        return log_error("read-wait for %s got zero", cfg->name);
    }

    if (verbose) {
        log_info("Container (name=%s pid=%d) recv-go", cfg->name, cfg->child_pid);
    }

    return 0;;
}

static int child_wake_sync(struct container_config *cfg)
{
    if (verbose)  {
        log_info("Container (name=%s pid=%d) send-ready", cfg->name, cfg->child_pid);
    }

    ssize_t nw = write_sync(cfg->ready_write_fd);

    if (nw == -1) {
        return log_errno("write-ready_fd for %s failed", cfg->name);
    }
    if (nw != 1) {
        return log_error("write-ready_fd for %s got zero", cfg->name);
    }

    // close pipe ends we no longer need
    if (close_fd(&cfg->ready_write_fd) != 0) {
        return log_errno("close ready_write_fd for %s failed", cfg->name);
    }
    if (close_fd(&cfg->go_read_fd) != 0) {
        return log_errno("close go_read_fd for %s failed", cfg->name);
    }

    return 0;;
}

int parent_close_go(struct container_config *cfg)
{
    int num_err = 0;
    
    if (close_fd(&cfg->go_read_fd) != 0) {
        log_errno("parent close %s go_read_fd failed", cfg->name);
        num_err++;
    }

    if (close_fd(&cfg->ready_write_fd) != 0) {
        log_errno("parent close %s reay_write_fd failed", cfg->name);
        num_err++;
    }

    return num_err;
}

int create_pipes(struct container_config *cfg)
{
    int fds[2];

    // create go sync pipe
    if (pipe(fds) == -1)  {
        return log_errno("create go-pipe for %s failed", cfg->name);
    }

    cfg->go_read_fd = fds[0];
    cfg->go_write_fd = fds[1];

    if (pipe(fds) == -1) {
        return log_errno("create ready_pipe for %s failed", cfg->name);
    }

    cfg->ready_read_fd = fds[0];
    cfg->ready_write_fd = fds[1];

    // all done
    return 0;
}

int copy_file(const char *src, const char *dst) 
{
    int rc = -1;

    int src_fd = open(src, O_RDONLY);
	if (src_fd < 0)
		return log_errno("copy_file open src %s failed", src);

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (dst_fd < 0) {
    	close(src_fd);
		return log_errno("copy_file open dst %s failed", dst);
    }

    struct stat stat_buf;
    if (fstat(src_fd, &stat_buf) == -1) {
		log_errno("copy_file fstat src_fd %d failed", src_fd);
        goto close_all;
	}

	if (!S_ISREG(stat_buf.st_mode)) {
		log_errno("copy_file src %s not a file", src);
        goto close_all;
	}	

	// XXX file size can be 0
	off_t offset = 0;
	while (offset < stat_buf.st_size) {
    	ssize_t sent = sendfile(dst_fd, src_fd, &offset, stat_buf.st_size - offset);
    	if (sent < 0) {
        	if (errno == EINTR) continue; // Interrupted, try again
			log_errno("copy_file send %s failed", src);
            goto close_all;
    	}
		if (sent == 0) {
			log_error("copy_file send %s eof", src);
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
		return log_errno("mount_cmd touch %s failed", rootfs_path);
    }
	close(fd); 

    // create a writable bind-mount
    if (mount(host_path, rootfs_path, NULL, MS_BIND, NULL) < 0) {
        int _errno = errno;
        unlink(rootfs_path);
        errno = _errno; 
		return log_errno("mount_cmd bind mount %s failed", rootfs_path);
    }

    // update bind-mount to read-only
    if (mount(NULL, rootfs_path, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0) {
        int _errno = errno;
		log_errno("mount_cmd remount read-only %s failed", rootfs_path);
        umount2(rootfs_path, MNT_DETACH); 
        unlink(rootfs_path);
        errno = _errno; 
		return log_errno("mount_cmd bind mount %s failed", rootfs_path);
    }

    // all done
    return 0;
}


static inline char *get_rootfs(struct container_config *cfg)
{
    return cfg->use_subdir ? cfg->rootfs_path : cfg->root_path;
}

#ifdef SECURITY
static int setup_priv(struct container_config *cfg)
{
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1) {
        return log_errno("prctl set-nonnew_privs failed for container %s", cfg->name);
    }

    if (drop_capabilities() != 0)  {
        return log_errno("drop-apabilities failed for container %s", cfg->name);
    }

    if (apply_seccomp() != 0) {
        return log_errno("apply-seccomp failed for container %s", cfg->name);
    }

    return 0;
}
#else
static int setup_priv(struct container_config *cfg) { return 0; }

#endif

static int container_start(void *arg)
{
    struct container_config *cfg = arg;

    cfg->child_pid = getpid();
    if (verbose) {
        log_info("Container (name=%s pid=%d) started", cfg->name, cfg->child_pid);
    }

    if (setup_priv(cfg) != 0) _exit(1);
    if (set_identity(cfg->name) != 0) _exit(2);
    if (child_wait_sync(cfg) != 0) _exit(3);

    if (set_rootfs(get_rootfs(cfg)) !=0) _exit(4);
    if (set_proc() != 0) _exit(5);

    if (cfg->need_network && create_network(cfg->veth_name, cfg->ip_addr) != 0) _exit(6);
    if (child_wake_sync(cfg) != 0) _exit(7);

    // XXX close remaing fds other than stdio,stdout,stderr
    if (syscall(SYS_close_range, 3, ~0U, 0) == -1) {
        log_errno("close_range failed");
        _exit(8); 
    }

    // finally run the cmd
    execv(cfg->exec_path, cfg->exec_argv);
    log_errno("child %s execv '%s' failed", cfg->name, cfg->exec_path);
    _exit(9);
}

int container_run(struct launcher_state *state, struct container_config *cfg)
{
    if (verbose) log_info("Launcher starting %s", cfg->name);

    if (create_pipes(cfg) != 0) {
        return -1;
    }

    // allocate a protected memory region for child stack
    // - never ever use malloc as child can corrupt it and parents heap
    // - linux stack grows downwards
    cfg->stack_size = 1024 * 1024;
    void *stack = mmap(NULL, cfg->stack_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0
    );
    if (stack == MAP_FAILED) {
        return log_errno("mmap stack %zu failed", cfg->stack_size);
    }
    cfg->stack = stack; // XXX MAP_FAILED may not be 0

    // setup clone flags
    int clone_flags = SIGCHLD | CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS;
    if (cfg->netns_mounted) {
        //  switch to child netns
        if (setns(cfg->netns_fd, CLONE_NEWNET) != 0) {
            return log_errno("netns-set(%d,'%s') for %s failed", cfg->netns_fd, cfg->netns_path, cfg->name);
        }
    }
    else if (cfg->need_network) {
        clone_flags |= CLONE_NEWNET;
    }

    // launch child
    cfg->child_pid = clone(container_start, cfg->stack + cfg->stack_size, clone_flags, cfg);
    if (cfg->child_pid == -1) {
        // failed ?
        int _errno = errno;
        // XXX retore the parnent netns
        setns(state->host_netns_fd, CLONE_NEWNET);
        errno = _errno;
        return log_errno("clone-child('%s','%s') failed", cfg->name, cfg->exec_path);
    }

    cfg->run = 1;
    state->num_run++;

    // XXX retore the parents netns
    if (setns(state->host_netns_fd, CLONE_NEWNET) != 0) {
        return log_errno("set-netns %s failed", HOST_NETNS_PATH);
    }

    // close our ends - child has a copy
    if (parent_close_go(cfg) != 0) {
        return -1;
    }

    // release overlay mount
    if (cfg->overlay_mounted) {
        if (umount2(cfg->rootfs_path, MNT_DETACH) < 0 && errno != EINVAL) {
            log_errno("container_run %s unmount overlay %s failed", cfg->name, cfg->rootfs_path);
        }
        cfg->overlay_mounted = 0;
        return -1;
    }

    // all done
    return 0;
}

void container_cleanup(struct container_config *cfg)
{
    if (cfg->run && cfg->child_pid > 0) {
        shutdown_pid(cfg->child_pid, 10000);
        cfg->run = 0;
    }

    // release all fds
    close_fd(&cfg->go_read_fd);
    close_fd(&cfg->go_write_fd);
    close_fd(&cfg->ready_read_fd);
    close_fd(&cfg->ready_write_fd);
    close_fd(&cfg->netns_fd);

    // release bind mount
    if (cfg->netns_mounted) {
        if (umount2(cfg->netns_path, MNT_DETACH));
        if (unlink(cfg->netns_path));
        cfg->netns_mounted = 0;
    }

    // release overlay mount
    if (cfg->overlay_mounted) {
        umount2(cfg->rootfs_path, MNT_DETACH);
        cfg->overlay_mounted = 0;
        if (cfg->cmd_mounted) {
            // XXX an rootfs unmout clears all mounts
            cfg->cmd_mounted = 0;
        }
    }

    // release cmd mount
    if (cfg->cmd_mounted) {
        umount2(cfg->dst_path, MNT_DETACH);
        cfg->cmd_mounted = 0;
    }

    // release stack memory
    if (cfg->stack) {
        munmap(cfg->stack, cfg->stack_size);
        cfg->stack = NULL;
    }

    // release name,exec_path,...
    if (cfg->name) free(cfg->name);
    if (cfg->cmd_path) free(cfg->cmd_path);
    if (cfg->exec_path) free(cfg->exec_path);
    if (cfg->exec_argc) {
        for (int i = 0; i < cfg->exec_argc; i++) {
            free(cfg->exec_argv[i]);
        }
        free(cfg->exec_argv);
    }
    if (cfg->ip_addr) free(cfg->ip_addr);

    if (cfg->root_path) free(cfg->root_path);
    if (cfg->rootfs_path) free(cfg->rootfs_path);
    if (cfg->netns_path) free(cfg->netns_path);
    if (cfg->dst_path) free(cfg->dst_path);

    if (cfg->lower_path) free(cfg->lower_path);
    if (cfg->upper_path) free(cfg->upper_path);
    if (cfg->work_path) free(cfg->work_path);

    // all done
}


static char **parse_args(const char *exec_path, const char *args_str, int *argc_out) 
{
    wordexp_t p = { 0 };

    if (args_str && wordexp(args_str, &p, WRDE_NOCMD) != 0) {
        log_error("wordexp failed");
        if (argc_out) *argc_out = 0;
        return NULL;
    }

    char **argv = malloc((p.we_wordc + 2) * sizeof(char *));
    if (argv == NULL) {
        log_error("malloc failed");
        return NULL;
    }

    argv[0] = strdup(exec_path);
    for (int i = 0; i < p.we_wordc; i++) {
        argv[i + 1] = strdup(p.we_wordv[i]);
    }
    argv[p.we_wordc + 1] = NULL; 
    if (argc_out) *argc_out = p.we_wordc + 1;

    wordfree(&p); 

    return argv;
}


static struct container_config *add_config(
    struct launcher_state *state,
    const char *name, 
	const char *cmd_path,
    const char *exec_path, 
    const char *exec_args,
    const char *ip_addr)
{
    struct container_config *cfg;

    if (!name) return log_errorn("Missing container name");
    if (!cmd_path) return log_errorn("Missing cmd_name");
    if (!exec_path) return log_errorn("Missing exec_path");

    if (state->num_cfg >= state->max_cfg) { 
        log_error("Too many containers - num %d >= max %d", state->num_cfg, state->max_cfg);
        return NULL;
    }

    cfg = &state->configs[state->num_cfg++];

    // init - XXX all fds must be set to -1
    memset(cfg, 0, sizeof(*cfg));
    cfg->go_read_fd  = -1;
    cfg->go_write_fd = -1;
    cfg->ready_read_fd = -1;
    cfg->ready_write_fd = -1;
    cfg->netns_fd = - 1;

    cfg->name = strdup(name);
    cfg->cmd_path = strdup(cmd_path);
    cfg->exec_path = strdup(exec_path);
    cfg->exec_argv = parse_args(exec_path, exec_args, &cfg->exec_argc);

    if (ip_addr) {
        cfg->ip_addr = strdup(ip_addr);
    }

    return cfg;
}

int create_netns(struct launcher_state *state, struct container_config *cfg)
{
    // generate name e.g "name-ns"
    char *suffix = state->netns_suffix ?: "";
    int rc = snprintf(cfg->netns_name, sizeof(cfg->netns_name), "%s%s", cfg->name, suffix);
    if (rc < 0 || rc == 0 || rc >= sizeof(cfg->netns_name)) {
        return log_error("genname %s failed", cfg->name);
    }

    // generate path e,g "/var/run/netns/name-ns"
    cfg->netns_path = gen_path(state->netns_dir, cfg->netns_name);
    if (!cfg->netns_path) {
        return log_errno("genpath %s failed", cfg->netns_name);
    }

    // bind mount path
    cfg->netns_fd = mount_netns(cfg->netns_path);
    if (cfg->netns_fd == -1) {
        return log_error("mount_netns %s failed", cfg->netns_name);
    }
    cfg->netns_mounted = 1;

    log_info("Created network namespace: %s", cfg->netns_name);

    return 0;
}

static char *gen_id(char *buf, int len, const char *name)
{
    uint32_t id = dbj2a_hash_str(name) ^ (uint64_t) time(NULL);

    int rc = snprintf(buf, len, "%08x", id);
    if (rc < 0 || rc == 0 || rc >= len)  {
        return log_errnon("gen_id failed for %s", name);
    }

    return buf;
}




char *create_subdir(const char *dir, const char *subdir, mode_t mode)
{
    char *path = gen_path(dir, subdir);
    if (!path) {
        log_errno("create_subdir genpath %s failed", subdir);
        return NULL;
    }

    int rc = create_path(path, mode);
    if (rc != 0) {
        free(path);
        path = NULL;
    }

    return path;
}

int create_subdirs(struct launcher_state *state, struct container_config *cfg)
{
    if (!state->use_subdirs) return 0;

    if (!(cfg->rootfs_path = create_subdir(state->storage_dir, "rootfs", 0755))) return -1;
    if (!state->use_overlay) return 0;

    if (!(cfg->lower_path = create_subdir(state->storage_dir, "lower", 0755)));
    if (!(cfg->upper_path = create_subdir(state->storage_dir, "upper", 0755)));
    if (!(cfg->work_path = create_subdir(state->storage_dir, "work", 0755)));

    return 0;
}

int create_root(struct launcher_state *state, struct container_config *cfg)
{
    char tmp[10];
    char *name;

    name = state->use_name_id ? gen_id(tmp, sizeof(tmp), cfg->name) : cfg->name;

    cfg->root_path = gen_path(state->storage_dir, name);
    if (!cfg->root_path) return log_errno("create_root genpath %s failed", name);

    RUN(create_dir(cfg->root_path, state->dir_mode, 1));

    return 0;
}

int mount_overlay(struct launcher_state *state, struct container_config *cfg)
{
    int rc;
	char *opts = NULL;

    if (!state->use_overlay) return 0;

	rc = asprintf(&opts, 
        "lowerdir=%s,upperdir=%s,workdir=%s", 
		cfg->lower_path, cfg->upper_path, cfg->work_path
	);

	if (rc == -1) {
		log_errno("mount overlayfs genopts failed");
        return -1;
	}

	rc = mount("overlay", cfg->rootfs_path, "overlay", 0, opts);
	if (rc == -1) {
		log_errno("mount overlayfs %s failed", cfg->rootfs_path);
	}
    cfg->overlay_mounted = 1;

	free(opts);

    return rc;
}

// copy files from host into container filesystem
int copy_files(struct launcher_state *state, struct container_config *cfg)
{
    if (verbose) {
        log_info("Launcher copy-files (name=%s, cmd=%s)", cfg->name, cfg->cmd_path);
    }

    char *cmd_path = NULL, *dst_path = NULL, *dst_dir = NULL;
    int rc = 0;

    // check src path exists
    cmd_path = realpath(cfg->cmd_path, NULL);
    if (!cmd_path) {
        log_errno("realpath %s failed", cfg->cmd_path);
        rc = -1;
        goto done;
    }

    // generate dst path - rootfs/exec_path
    dst_path = gen_path(get_rootfs(cfg), cfg->exec_path);
    if (!dst_path) {
        log_errno("gen_path %s failed", cfg->exec_path);
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
    rc = create_path_nocopy(dirname(dst_dir), state->dir_mode);
    if (rc != 0) goto done;

    // finally copy or mount file 
    if (state->use_cmd_mount) {
        rc = mount_cmd_file(cmd_path, dst_path);
        if (rc == 0) {
            cfg->cmd_mounted = 1;
            // need to store mount point
            cfg->dst_path = dst_path;
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

int check_network(struct launcher_state *state, struct container_config *cfg)
{
    if (cfg->ip_addr && state->child_add_ip) { 
        cfg->need_network = 1;
    }

    return 0;
}

int open_host_netns(struct launcher_state *state)
{
    // XXX fd must be for this process only (O_CLOEXEC)
    state->host_netns_fd = open(HOST_NETNS_PATH, O_RDONLY | O_CLOEXEC);
    if (state->host_netns_fd == -1) {
        return log_errno("open host_netns failed");
    }

    return 0;
}

int setup_infrastucture(struct launcher_state *state)
{
    if (verbose) {
        log_info("Launcher setup_infrastucture");
    }

    RUN(open_host_netns(state));

    // create dirs
    RUN(create_path(state->netns_dir, state->dir_mode));
    RUN(create_path(state->storage_dir, state->dir_mode));
    RUN(create_path(state->runtime_dir, state->dir_mode));

    for (int i = 0; i < state->num_cfg; i++) {
        struct container_config *cfg = &state->configs[i];
        RUN(create_root(state, cfg));
        RUN(create_subdirs(state, cfg));
        RUN(mount_overlay(state, cfg));
        RUN(copy_files(state, cfg));
        RUN(check_network(state, cfg));
        RUN(create_netns(state, cfg));
    }

    // all done
    return 0;
}


static int set_cable_name(struct launcher_state *state, struct container_config *cfg)
{
    const char *prefix = state->cable_prefix;
    if (!prefix) prefix = "";

    int rc = snprintf(cfg->veth_name, sizeof(cfg->veth_name), "%s%s", prefix, cfg->name);
    if (rc < 0 || rc == 0 || rc >= sizeof(cfg->veth_name))
        return log_error("seT_cable_name: snprintf %d failed", rc);

    return 0;
}

int setup_network(struct container_config *cfg)
{
    if (verbose) {
        log_info("launcher setup-network (name=%s ipaddr=%s" , cfg->name, cfg->ip_addr);
    }

    RUN_CMD("nsenter -t %d -n ip link set %s name eth0", cfg->child_pid, cfg->veth_name);
    RUN_CMD("nsenter -t %d -n ip addr add %s/24 dev eth0", cfg->child_pid, cfg->ip_addr);
    RUN_CMD("nsenter -t %d -n ip link set lo up", cfg->child_pid);
    RUN_CMD("nsenter -t %d -n ip link set eth0 up", cfg->child_pid);

    cfg->need_network = 0;

    return 0;
}

int create_cable(struct launcher_state *state, 
    struct container_config *x, 
    struct container_config *y)
{
    if (verbose) {
        log_info("Launcher create-cable (left=%s, right=%s)", x->name, y->name);
    }

    RUN(set_cable_name(state, x));
    RUN(set_cable_name(state, y));

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


int check_reaped(struct launcher_state *state, struct container_config *cfg)
{
    char why[40];

    if (!child_reaped(cfg->status)) return 0;

    cfg->run = 0;
    state->num_run--;

    if (WIFEXITED(cfg->status)) {
        int ec = WEXITSTATUS(cfg->status);
        snprintf(why, sizeof(why), "exit_code %d", ec);
    }
    else if (WIFSIGNALED(cfg->status)) {
        int sig = WTERMSIG(cfg->status);
        snprintf(why, sizeof(why), "signal %d (%s)", sig, strsignal(sig));
    }
    else {
        snprintf(why, sizeof(why), "status 0x%08x", cfg->status);
    }

    return log_error("Container '%s' died (pid=%d why=%s)", cfg->name, cfg->child_pid, why);
}

int check_wait(struct launcher_state *state, struct container_config *cfg)
{
    pid_t res = waitpid(cfg->child_pid, &cfg->status, WNOHANG);
    if (res == -1) {
        return log_errno("waipid for %s failed", cfg->name);
    }
    if (res == cfg->child_pid && check_reaped(state, cfg) != 0) {
        return -1;
    }

    return 0;
}

int state_wake_sync(struct launcher_state *state, struct container_config *cfg)
{
    // check if chlld still running
    if (check_wait(state, cfg) != 0) {
        return -1;
    }

    if (verbose) {
        log_info("Launcher (name=%s pid=%d) send-go", cfg->name, cfg->child_pid);
    }

    // wake up child
    ssize_t nw = write_sync(cfg->go_write_fd);
    if (nw != 1) {
        int _errno = errno;
        close_fd(&cfg->go_write_fd);
        errno = _errno;
        return log_errno("write wake-sync for %s failed", cfg->name);
    }

    // relese pipe
    if (close_fd(&cfg->go_write_fd) != 0) {
        return log_errno("close wait-sync for %s failed", cfg->name);
    }

    return 0;
}

int state_wait_sync(struct launcher_state *state, struct container_config *cfg)
{
    // check if chlld still running
    if (check_wait(state, cfg) != 0) {
        return -1;
    }

    // wait for child
    ssize_t nr = read_sync(cfg->ready_read_fd);
    if (nr == -1)  {
        int _errno = errno;
        close_fd(&cfg->ready_read_fd);
        errno = _errno;
        return log_errno("read wait-sync for %s failed", cfg->name);
    }

    // relese pipe
    if (close_fd(&cfg->ready_read_fd) != 0) {
        return log_errno("close wait-sync %s failed", cfg->name);
    }

    if (verbose) {
        log_info("Launcher (name=%s pid=%d) recv-ready", cfg->name, cfg->child_pid);
    }

    return 0;
}

int sync_containers(struct launcher_state *state)
{
    if (verbose) {
        log_info("Launcher sync %d containers %s", 
            state->num_cfg,
            state->start_order ? "sequential" : "parallel");
    }

    if (state->start_order) {
        // sequential sync
        for (int i = 0; i < state->num_cfg; i++) {
            RUN(state_wake_sync(state, &state->configs[i]));
            RUN(state_wait_sync(state, &state->configs[i]));
            sleep(state->start_delay);
        }
    }
    else {
        // parallel sync
        for (int i = 0; i < state->num_cfg; i++) {
            RUN(state_wake_sync(state, &state->configs[i]));
        }
        for (int i = 0; i < state->num_cfg; i++) {
            RUN(state_wait_sync(state, &state->configs[i]));
        }
    }

    return 0;
}

int start_containers(struct launcher_state *state)
{
    if (verbose) {
        log_info("Launcher starting %d containers", state->num_cfg);
    }

    for (int i = 0; i < state->num_cfg; i++) {
        RUN(container_run(state, &state->configs[i]));
    }

    return 0;
}


static struct container_config *find_child(struct launcher_state *state, pid_t pid)
{
    for (int i = 0; i < state->num_cfg; i++) {
        if (state->configs[i].child_pid == pid) {
            return &state->configs[i];
        }
    }

    return NULL;
}

int wait_containers(struct launcher_state *state)
{
    int status;
    struct container_config *cfg;

    while (state->num_run > 0) {
        pid_t pid = waitpid(-1, &status, 0); 
        if (pid == 0) continue;
        if (pid == -1) {
            // waitpid failed
            if (errno == EINTR) continue;
            if (errno == ECHILD) {
                // no more children - stop now
                for (int i = 0; i < state->num_cfg; i++) {
                    state->configs[i].run = 0;
                }
                state->num_run = 0;
                break;
            }
            return log_errno("waitpid failed");;
        }
        cfg = find_child(state, pid);
        if (!cfg) {
            // XXX - not ours ?
            log_info("waitpid reaped unknown child pid %d", pid);
            continue;
        }
        // check if running 
        cfg->status = status;
        if (check_reaped(state, cfg) != 0) {
            return -1;
        }
    }

    return 0;
}

char *validate_dir(const char *key, struct str_slice dir) 
{
    char *path = realpath(dir.ptr, NULL);
    if (!path) {
        return log_errnon("%s realpath %s failed");
    }

    // Check if the path exists
    int rc = -1;

    struct stat st;
    if (stat(path, &st) != 0) {
       log_error("%s path %s does not exist", key, dir);
       goto done;
    }

    // check file is a directory
    if (!S_ISDIR(st.st_mode)) {
        log_error("%s path %s is not a dir", key, dir);
        goto done;
    }

    // check dir is accesible
    if (access(path, R_OK | X_OK) != 0) {
        log_error("%s path %s is not accesible", key, dir);
        goto done;
    }

    // okay
    rc = 0;

done:
    if (rc != 0) free(path);
    // all done
    return path;
}

void print_usage(struct launcher_state *state, const char *cmd)
{
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

    printf("\nExample:\n");
    printf("  %s startorder=1 startdelay=5\n", prog_name);
}

int parse_cmd_line(struct launcher_state *state, int argc, char *argv[])
{
    int num_err = 0;

    for (int i = 1; i < argc; i++) {
        // get key ["=value"]
		struct str_slice opt = slice_make_cstr(argv[i]);
		struct str_slice val = slice_split(&opt, '=');

        if (slice_cmp_cstr(opt, STR_LIT("-help"))) {
            print_usage(state, argv[0]);
            num_err++;
        }
        else if (slice_cmp_cstr(opt, STR_LIT("-v"))) {
            // log everthtng
            verbose = 1;
        }
        else if (slice_cmp_cstr(opt, STR_LIT("rootfs"))) {
            state->rootfs_dir = validate_dir("rootfs", val);
            if (!state->rootfs_dir) {
                num_err++;
            }
		}
        else if (slice_cmp_cstr(opt, STR_LIT("srcdir"))) {
            state->src_dir = validate_dir("srcdir", val);
            if (!state->src_dir) {
                num_err++;
            }
        }
        else if (slice_cmp_cstr(opt, STR_LIT("netnsdir"))) {
            state->netns_dir = validate_dir("netnsdir", val);
            if (!state->netns_dir) {
                num_err++;
            }
        }
        else if (slice_cmp_cstr(opt, STR_LIT("startorder"))) {
            state->start_order = atoi(val.ptr) != 0;
        }
        else if (slice_cmp_cstr(opt, STR_LIT("startdelay"))) {
            state->start_delay = atoi(val.ptr);
            if (state->start_delay < 0) {
                log_error("startdelay must greater than 0");
                num_err++;
            }
        }
    }

	return num_err;
}

void state_deinit(struct launcher_state *state)
{
    for (int i = 0; i < state->num_cfg; i++) {
        container_cleanup(&state->configs[i]);
    }
    state->num_cfg = 0;

    close_fd(&state->host_netns_fd);

    if (state->cur_dir) free(state->cur_dir);
    if (state->src_dir) free(state->src_dir);

    if (state->netns_dir) free(state->netns_dir);
    if (state->runtime_dir)  free(state->runtime_dir);
    if (state->storage_dir) free(state->storage_dir);
    if (state->rootfs_dir) free(state->rootfs_dir);

    if (state->netns_suffix) free(state->netns_suffix);
    if (state->cable_prefix) free(state->cable_prefix);

}

int init_state(struct launcher_state *state, int argc, char *argv[])
{
    // init
    memset(state, 0, sizeof(*state));
    state->host_netns_fd = -1;

    // FIXME do we need this ?
    signal(SIGPIPE, SIG_IGN);

    state->max_cfg = MAX_CONFIG;
    state->start_delay = START_DELAY;
    state->start_order = START_ORDER;

    // setup default dirs
    char *cwd = getcwd(NULL, 0);
    if (!cwd)  {
        return log_errno("get_cwd failed");
    }

    state->cur_dir = gen_path(cwd, "mylauncher");
    free(cwd);
    if (!state->cur_dir) {
        return log_error("cur_dir");
    }

    state->netns_dir = gen_path(state->cur_dir, "netns_dir");
    state->runtime_dir = gen_path(state->cur_dir, "run_dir");
    state->storage_dir = gen_path(state->cur_dir, "store_dir");

    state->netns_suffix = strdup("-ns");
    state->cable_prefix = strdup("veth-");

    state->dir_mode = STANDARD_MODE;

    return parse_cmd_line(state, argc, argv);
}

int main(int argc, char *argv[])
{
    struct launcher_state state;
    struct container_config *client, *server;

    if (init_state(&state, argc, argv) != 0) goto cleanup;
    server = add_config(&state, "db", "db/server", "/bin/server", NULL, "10.0.0.1");
    client = add_config(&state, "client", "client/client", "/bin/client", "10.0.0.1", "10.0.0.2");
    if (!client || !server) goto cleanup;

    if (setup_infrastucture(&state) != 0) goto cleanup;
    if (start_containers(&state) != 0) goto cleanup;
    if (create_cable(&state, client, server) != 0) goto cleanup;
    if (sync_containers(&state) != 0) goto cleanup;
    if (wait_containers(&state) != 0) goto cleanup;

cleanup:   
    state_deinit(&state);

    // all done
    return 0;
}
