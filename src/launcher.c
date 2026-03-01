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
#include <fcntl.h>
#include "util.h"

#define RUNTIME_DIR "/run/asimple_launcher"
#define STORAGE_DIR "/var/lib/asimple_launcher"
#define NETNS_DIR "/var/run/netns"
#define MAX_CONFIG 10

#define LOG_ERR(fmt, ...) do { \
    int _errno = errno; \
    fprintf(stderr, \
        "[%s:%d] ERROR: " fmt ": %s (errno: %d)\n", \
        __func__, __LINE__,  ##__VA_ARGS__, \
        strerror(_errno), _errno); \
} while(0)

void log_status(const char *fmt, ...)
{
    va_list args;  

    va_start(args, fmt);
    fprintf(stdout, "[+] ");
    vfprintf(stdout, fmt, args);
    va_end(args);
    fprintf(stdout, "\n");
}

bool str_starts_with(const char *str, const char *prefix) {

    while (*prefix) {
        if (*prefix++ != *str++) return false;
    }

    return true;
}


static int run_cmd(const char *fmt, ...)
{
    va_list args;
    char cmd[4096];
    int rc;

    va_start(args, fmt);
    rc = vsnprintf(cmd, sizeof(cmd), fmt, args);
    va_end(args);
    if (rc < 0) {
        LOG_ERR("vsnprintf failed");
        return -1;
    }

    rc = system(cmd);
    if (rc == -1) {
        LOG_ERR("system(%s) failed", cmd);
        return errno;
    }

    if (!WIFEXITED(rc)) {
        LOG_ERR("cmd (%s) interupted", cmd);
        return -1;
    }

    int ec = WEXITSTATUS(rc);
    if (ec != 0) {
        LOG_ERR("cmd (%s) failed %d", cmd, ec);
        return -1;
    }

    // all done
    return 0;
}


#define RUN_CMD(x, ...) do { \
    int rc = run_cmd(x,  ##__VA_ARGS__) ; \
    if (rc != 0) return rc; \
} while(0);

#define RUN(x) do { \
    int rc = (x); \
    if (rc != 0) return rc; \
} while(0);

int create_dir(const char *path)
{
    int rc = mkdir(path, 0755);

    if (rc == -1 && errno != EEXIST) {
        LOG_ERR("mkdir %s failed", path);
        return -1;
    }

    return 0;
}

int create_dirs(const char *path)  
{
    char tmp[PATH_MAX];
    int nw = snprintf(tmp, sizeof(tmp), "%s", path);
    if (nw < 0 || nw == 0 || nw >= sizeof(tmp)) {
        LOG_ERR("mkdir %s failed", path);
		return -1;
	}

    if (tmp[nw - 1] == '/') tmp[nw - 1] = 0; 

    // now traverse the path string
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0; // Temporarily terminate string
            if (create_dir(tmp) != 0) {
                return -1;
            }
            *p = '/'; // Restore slash
        }
    }

    // Create last dir on path
    return create_dir(tmp);
}

static int mount_netns(const char *netns_path)
{
    // creat path e.g touch "/var/run/netns/name"
    int fd = open(netns_path, O_RDONLY | O_CREAT | O_EXCL, 0600);
    if (fd == -1) {
        LOG_ERR("open %s failed", netns_path);
        return errno;
    }
    close(fd);

    // spawn a child for bind mount
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERR("fork for bind mount failed");
        return -errno;
    }

    if (pid == 0) {
        // inside child - bind mount the file 
        // i.e mount --bind /proc/self/ns/net /var/run/netnts/name
        if (unshare(CLONE_NEWNET) < 0) _exit(1);
        int rc = mount("/proc/self/ns/net", netns_path, "none", MS_BIND, NULL);
        if (rc == -1) {
            LOG_ERR("bind mount %s failed", netns_path);
            rc = EXIT_FAILURE;
        }
        // done
        _exit(rc);
    }

    // parent 
    int status; 
    pid_t p = waitpid(pid, &status, 0);
    if (p == -1) {
        LOG_ERR("mount_netns:waitpid(%d) failed", pid);
        return -1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LOG_ERR("mount_netns: child failed to bind mount");
        return -1;
    }

    // all done
    return 0;
}

static int create_veth(const char *container, char *veth, int veth_len)
{
    uint32_t id = dbj2a_hash_str(container) & 0xffffffff;
    int rc, salt = 10;
    do {
        rc = snprintf(veth, veth_len, "vth%08x%d", id, salt);
        if (rc < 0) {
            LOG_ERR("snprintf: veth name failed");
            return -1;
        }
        if (rc == 0 || rc >= veth_len) {
            LOG_ERR("snprintf: incorrct len %d", rc);
            return -1;
        }
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

    log_status("Created veth %s", veth);

    return 0;
}

static int set_identity(const char *name)
{
    int rc = sethostname(name, strlen(name));

    if (rc == -1) {
        LOG_ERR("sethostname %s failed", name);
        return -1;
    }

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
    int rc;

    // - make mount space private 
    rc = mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    if (rc == -1) { 
        LOG_ERR("private mount failed");
        return rc;
    }

    // - create new mount point
    rc = mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL); 
    if (rc == -1) {
        LOG_ERR("bind mount roofs %s failed", rootfs);
        return rc;
    }

    // - change dir to new mount point
    rc = chdir(rootfs);
    if (rc == -1) {
        LOG_ERR("chdir rootfs %s", rootfs);
        return rc;
    }

    // pivot_root to new mount point (stack old_root)
    rc = syscall(SYS_pivot_root, ".", ".");
    if (rc == -1) {
        LOG_ERR("pivot_root failed");
        return rc;
    }

    // - change root dir to new mount  task_struct (root ptr)
    rc = chroot(".");
    if (rc == -1) {
        LOG_ERR("chroot failed");
        return rc;
    }

    // - change curret dir to new root - task_struct (cwd ptr)
    rc = chdir("/");
    if (rc == -1) {
        LOG_ERR("chdir to / failed");
        return rc;
    }

    rc = umount2("/", MNT_DETACH); 
    if (rc == -1) {
        LOG_ERR("unmount2 / failed");
        return rc;
    }

    // all done
    return 0;
}

static int set_proc(void)
{
    // new PID namespace - create new /proc
    int rc = mkdir("/proc", 0755);
    if (rc == -1) {
        LOG_ERR("mkdir /proc failed");
        return rc;
    }

    mount("proc", "/proc", "proc", 0, NULL);
    if (rc == -1) {
        LOG_ERR("mount /proc faild");
        return rc;
    }

    return 0; 
}

// bring up child containers lo,eth0 and ip address
int create_network(const char *veth, const char *ip_addr)
{
    RUN_CMD("ip link set %s name eth0", veth); 
    RUN_CMD("ip link set lo up");
    RUN_CMD("ip link set eth0 up");

    RUN_CMD("ip addr add %s dev eth0", ip_addr);

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

    return nw;
}

static inline int close_fd(int *fd)
{
    int rc = 0;

    if (*fd != -1) {
        if (close(*fd) != 0) {
            rc = -1;
        }
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

static int child_setns(const char *netns_path)
{
    int fd = open(netns_path, O_RDONLY);
    if (fd == -1) {
        LOG_ERR("open netns %s failed", netns_path);
        return fd;
    }

    // switch task into network namespace
    int rc = setns(fd, CLONE_NEWNET);
    if (rc == -1) {
        LOG_ERR("setns %s failed", netns_path);
        return rc;
    }

    close(fd);

    return 0;
}

struct container_config {
    char *name;   // container name
    char *rootfs_path;     // For the bind mount and pivot_root()
    char *netns_path; // bind mounted network namespae
    char netns_name[IFNAMSIZ]; // network namespace name
    char *cmd_path;  //  location of cmd
    char *exec_path;  // process to lanuch
    char **exec_argv; // command line args
    int exec_argc;
    char veth[IFNAMSIZ];  // container eth0 link
    char *ip_addr;      // ip addr to add to veth
    // parent sync child
    int go_read_fd;    // child reads
    int go_write_fd;   // parent writes
    // child sync with parent
    int ready_read_fd; // parent reads
    int ready_write_fd; // child writes
    void *stack;       // passed to clone
    size_t stack_size;
    pid_t child_pid;   // value returned by clone
    int exit_status;
    unsigned int need_network : 1; // configure network
    unsigned int run : 1; // clone child is active
    unsigned int netns : 1;
    // waitpid flags
    unsigned int exit : 1;
    unsigned int signalled : 1;
    unsigned int stopped : 1;
    unsigned int continued : 1;
};


static int child_close_go(struct container_config *cfg)
{
    int num_err = 0;

    if (close_fd(&cfg->go_write_fd) != 0) {
        LOG_ERR("child close %s go_write_fd failed", cfg->name);
        num_err++;
    }

    if (close_fd(&cfg->ready_read_fd) != 0) {
        LOG_ERR("child close %s ready_read_fd failed", cfg->name);
        num_err++;
    }

    return num_err;;
}

static int child_close_ready(struct container_config *cfg)
{
    int num_err = 0;

    if (close_fd(&cfg->ready_write_fd) != 0) {
        LOG_ERR("child %s close ready_write_fd failed", cfg->name);
        num_err++;
    }

    if (close_fd(&cfg->go_read_fd) != 0) {
        LOG_ERR("child %s go_read_fd failed", cfg->name);
        num_err++;
    }

    return num_err;;
}

static int child_go_sync(struct container_config *cfg)
{
    ssize_t nr;

    nr = read_sync(cfg->go_read_fd);

    if (nr == -1) {
        LOG_ERR("child read_sync failed %s %d", cfg->name, cfg->go_read_fd);
        return -1;
    }

    if (nr != 1) {
        LOG_ERR("child %s go_read_fd got zero ", cfg->name);
        return -1;
    }

    return 0;;
}

static int child_ready_sync(struct container_config *cfg)
{
    ssize_t nw;

    nw = write_sync(cfg->ready_write_fd);

    if (nw == -1) {
        LOG_ERR("child write_sync failed %s %d", cfg->name, cfg->ready_write_fd);
        return -1;
    }

    if (nw != 1) {
        LOG_ERR("child %s ready_write_fd got zero ", cfg->name);
        return -1;
    }

    return 0;;
}

int parent_close_go(struct container_config *cfg)
{
    int num_err = 0;
    
    if (close_fd(&cfg->go_read_fd) != 0) {
        LOG_ERR("parent close %s go_reade_fd failed", cfg->name);
        num_err++;
    }

    if (close_fd(&cfg->ready_write_fd) != 0) {
        LOG_ERR("parent close %s reay_write_fd failed", cfg->name);
        num_err++;
    }

    return num_err;
}

int create_pipes(struct container_config *cfg)
{
    int fds[2];

    // create go sync pipe
    if (pipe(fds) == -1) {
        LOG_ERR("create go pipe failed");
        return -1;
    }

    cfg->go_read_fd = fds[0];
    cfg->go_write_fd = fds[1];

    if (pipe(fds) == -1) {
        LOG_ERR("create ready pipe failed");
        return -1;
    }

    cfg->ready_read_fd = fds[0];
    cfg->ready_write_fd = fds[1];

    return 0;
}

int copy_file(const char *src, const char *dst) 
{
    int src_fd = open(src, O_RDONLY);
	if (src_fd < 0) {
		LOG_ERR("copy_file open src %s failed", src);
		return -1;
	}

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (dst_fd < 0) {
		LOG_ERR("copy_file open dst %s failed", dst);
		return -1;
	}

    struct stat stat_buf;
    if (fstat(src_fd, &stat_buf) == -1) {
		LOG_ERR("copy_file fstat src_fd %d failed", src_fd);
    	close(src_fd);
    	close(dst_fd);
		return -1;
	}

	if (!S_ISREG(stat_buf.st_mode)) {
		LOG_ERR("copy_file src %s not a file", src);
    	close(src_fd);
    	close(dst_fd);
		return -1;
	}	

	// XXX file size can be 0
	off_t offset = 0;
	while (offset < stat_buf.st_size) {
    	ssize_t sent = sendfile(dst_fd, src_fd, &offset, stat_buf.st_size - offset);
    	if (sent < 0) {
        	if (errno == EINTR) continue; // Interrupted, try again
			LOG_ERR("copy_file send %s failed", src);
        	return -1;
    	}
		if (sent == 0) {
			LOG_ERR("copy_file send %s eof", src);
			break;
		}
		// sendfile updates offset
	}

    close(src_fd);
    close(dst_fd);

	// check all sent
    return offset == stat_buf.st_size ? 0 : -1;
}

int check_valid_dir(const char *path) 
{
    struct stat st;

    // Check if the path exists
    if (stat(path, &st) != 0) {
		LOG_ERR("check_valid_dir %s failed", path);
        return -1; 
    }

    // check file is a directory
    if (!S_ISDIR(st.st_mode)) {
		LOG_ERR("chec_valid_dir %s not a dir", path);
        return 0;
    }

    // 3. Check for Read (R_OK) and Execute (X_OK) permissions
    // Note: Directories need 'execute' permission to be "entered" or listed.
    if (access(path, R_OK | X_OK) != 0) {
		LOG_ERR("check_valid_dir %s not accesible", path);
        return -1;
    }

    return 0;
}


// note child is running inside new task_struct
static int child_start(void *arg)
{
    struct container_config *cfg = arg;

    // close pipe ends we don't use
    if (child_close_go(cfg) != 0) _exit(1);
    if (set_identity(cfg->name) != 0) _exit(2);

    // wait for parent to configure netns,veth, cgroup,...
    if (child_go_sync(cfg) != 0) _exit(3);

    if (cfg->netns_path && child_setns(cfg->netns_path) != 0) _exit(4);
    if (set_rootfs(cfg->rootfs_path) !=0) _exit(5);
    if (set_proc() != 0) _exit(6);

    if (cfg->need_network && create_network(cfg->veth, cfg->ip_addr) != 0) _exit(7);
    if (child_ready_sync(cfg) != 0) _exit(6);
    if (child_close_ready(cfg) != 0) _exit(7);

    // finally exec the program
    execv(cfg->exec_path, cfg->exec_argv);
    LOG_ERR("execv %s failed", cfg->exec_path);
    _exit(6);
}


int container_start(struct container_config *cfg)
{
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
        LOG_ERR("mmap stack %zu failed", cfg->stack_size);
        return -1;
    }
    cfg->stack = stack; // XXX MAP_FAILED may not be 0

    // launch child
    int clone_flags = SIGCHLD | CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS;
    if (!cfg->netns_path) clone_flags |= CLONE_NEWNET;
    cfg->child_pid = clone(child_start, cfg->stack + cfg->stack_size, clone_flags, cfg);
    if (cfg->child_pid == -1) {
        LOG_ERR("clone child(%s,%s) failed", cfg->name, cfg->exec_path);
        return -1;
    }
    cfg->run = 1;

    // close our ends - child has a copy
    if (!parent_close_go(cfg) != 0) {
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

    // release sync pipes
    close_fd(&cfg->go_read_fd);
    close_fd(&cfg->go_write_fd);
    close_fd(&cfg->ready_read_fd);
    close_fd(&cfg->ready_write_fd);

    // release bind mount
    if (cfg->netns_path) {
        umount2(cfg->netns_path, MNT_DETACH); 
        unlink(cfg->netns_path);
        free(cfg->netns_path);
        cfg->netns_path = NULL;
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
    if (cfg->rootfs_path) free(cfg->rootfs_path);

    // all done
}

int parent_go_sync(struct container_config *cfg)
{
    ssize_t nw;

    nw = write_sync(cfg->go_write_fd);
    if (nw != 1) {
        LOG_ERR("parent write go_sync %s %d failed", cfg->name, cfg->go_write_fd);
        close_fd(&cfg->go_write_fd);
        return -1;
    }

    if (close_fd(&cfg->go_write_fd) != 0) {
        LOG_ERR("parent close go_sync %s failed", cfg->name);
    }

    return 0;
}

int parent_ready_sync(struct container_config *cfg)
{
    ssize_t nr;

    nr = read_sync(cfg->ready_read_fd);
    if (nr != 1)  {
        LOG_ERR("parent read ready_sync %s %d failed", cfg->name, cfg->ready_read_fd);
        close_fd(&cfg->ready_read_fd);
        return -1;
    }

    if (close_fd(&cfg->ready_read_fd) != 0) {
        LOG_ERR("parnt close ready_sync %s failed", cfg->name);
    }

    return 0;
}

static char **parse_args(const char *args_str, int *argc_out) 
{
    if (!args_str) {
        if (argc_out) *argc_out = 0;
        return NULL;
    }
    
    wordexp_t p;
    if (wordexp(args_str, &p, WRDE_NOCMD) != 0) {
        LOG_ERR("wordexp failed");
        return NULL;
    }

    char **argv = malloc((p.we_wordc + 1) * sizeof(char *));
    for (int i = 0; i < p.we_wordc; i++) {
        argv[i] = strdup(p.we_wordv[i]);
    }
    argv[p.we_wordc] = NULL; 

    if (argc_out) *argc_out = 0;

    wordfree(&p); 

    return argv;
}


static int load_config(struct container_config *cfg,
    const char *name, 
    const char *cmd_path, 
    const char *exec_path, 
    const char *exec_args,
    const char *ip_addr)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->go_read_fd  = -1;
    cfg->go_write_fd = -1;
    cfg->ready_read_fd = -1;
    cfg->ready_write_fd = -1;

    if (!name || !!cmd_path || !exec_path) {
        LOG_ERR("config: need name,exec_path");
        return -1;
    }

    cfg->name = strdup(name);
    cfg->cmd_path = strdup(exec_path);
    cfg->exec_path = strdup(exec_path);
    cfg->exec_argv = parse_args(exec_args, &cfg->exec_argc);

    if (ip_addr) {
        cfg->ip_addr = strdup(ip_addr);
        cfg->need_network = 1;
    }

    return 0;
}

struct launcher_state {
    char *netns_dir;
    char *runtime_dir;
    char *storage_dir;
    struct container_config configs[MAX_CONFIG];
    int max_cfg;
    int num_cfg;
    int num_run;
    char *rootfs;
	char *bindir;
    char *netns_suffix;
    char *cable_prefix;
};

static struct container_config *add_config(
    struct launcher_state *state,
    const char *name, 
	const char *cmd_path,
    const char *exec_path, 
    const char *exec_args,
    const char *ip_addr)
{
    struct container_config *cfg;

    if (state->num_cfg >= state->max_cfg) return NULL;

    cfg = &state->configs[state->num_cfg++];

    if (load_config(cfg, name, cmd_path, exec_path, exec_args, ip_addr) != 0) {
        container_cleanup(cfg);
        state->num_cfg--;
        cfg = NULL;
    }

    return cfg;
}

int add_rootfs(struct launcher_state *state, const char *rootfs)
{
	if (check_valid_dir(rootfs) != 0) {
		return -1;
	}

	state->rootfs = strdup(rootfs);
	if (!state->rootfs) {
        LOG_ERR("add rootfs %s failed", rootfs);
		return -1;
	}

	return 0;
}

int add_bindir(struct launcher_state *state, const char *bindir)
{
	if (check_valid_dir(bindir) != 0) {
		return -1;
	}

	state->bindir = strdup(bindir);
	if (!state->bindir) {
        LOG_ERR("add bindir %s failed", bindir);
		return -1;
	}

	return 0;
}

int parse_cmd_line(struct launcher_state *state, int argc, char *argv[])
{
    for (int i = 0; i < argc; i++) {
		char *option = argv[i];
        if (str_starts_with(option, "rootfs=")) {
			option += strlen("rootfs=");
			RUN(add_rootfs(state, option));
		}
        if (str_starts_with(option, "bindir=")) {
			option += strlen("rootfs=");
			RUN(add_bindir(state, option));
        }
    }

	return 0;
}

void state_deinit(struct launcher_state *state)
{
    for (int i = 0; i < state->num_cfg; i++) {
        container_cleanup(&state->configs[i]);
    }
    state->num_cfg = 0;

    if (state->netns_dir) free(state->netns_dir);
    if (state->runtime_dir)  free(state->runtime_dir);
    if (state->storage_dir) free(state->storage_dir);
    if (state->netns_suffix) free(state->netns_suffix);
    if (state->cable_prefix) free(state->cable_prefix);
    if (state->rootfs) free(state->rootfs);
    if (state->bindir) free(state->bindir);

}


int init_state(struct launcher_state *state, int argc, char *argv[])
{
    memset(state, 0, sizeof(*state));

    state->max_cfg = MAX_CONFIG;
    state->netns_dir = strdup(NETNS_DIR);
    state->runtime_dir = strdup(RUNTIME_DIR);
    state->storage_dir = strdup(STORAGE_DIR);
    state->netns_suffix = strdup("-ns");
    state->cable_prefix = strdup("veth-");

    return parse_cmd_line(state, argc, argv);
}


int create_netns(struct container_config *cfg, const char *netns_dir, const char *suffix)
{
    char netns_path[PATH_MAX];

    // gen name e.g "name-ns"
    if (!suffix) suffix = "";
    int rc = snprintf(cfg->netns_name, sizeof(cfg->netns_name), "%s%s", cfg->name, suffix);
    if (rc < 0 || rc == 0 || rc >= sizeof(cfg->netns_name))  {
        LOG_ERR("snprintf failed for %s", cfg->name);
        return -1;
    }

    // gen path e,g "/var/run/netns/name-ns"
    rc = snprintf(netns_path, sizeof(netns_path), "%s/%s", netns_dir, cfg->netns_name);
    if (rc < 0 || rc == 0 || rc >= sizeof(netns_path))  {
        LOG_ERR("snprintf failed for %s", cfg->name);
        return -1;
    }
    cfg->netns_path = strdup(netns_path);
    if (!cfg->netns_path) {
        LOG_ERR("strdup netns_path %s failed", netns_path);
        return -1;
    }

    rc = mount_netns(cfg->netns_path);
    if (rc != 0) return rc;

    cfg->netns = 1;
    log_status("Created network namespace: %s", cfg->netns);

    return 0;
}

static uint32_t generate_id(const char *name)
{
    return dbj2a_hash_str(name) ^ (uint64_t) time(NULL);
}

int create_rootfs(struct container_config *cfg, const char *storeage_dir)
{
    char rootfs_path[PATH_MAX];
    uint32_t id = generate_id(cfg->name);

    int rc = snprintf(rootfs_path, sizeof(rootfs_path), "%s/%08x/rootfs", storeage_dir,id);
    if (rc < 0 || rc == 0 || rc >= sizeof(rootfs_path)) {
        LOG_ERR("create_rootfs: snprintf %d failed for %s", rc, cfg->name);
        return -1;
    }

    cfg->rootfs_path = strdup(rootfs_path);
    if (!cfg->rootfs_path) {
        LOG_ERR("create_roots: strdup failed");
        return -1;
    }

    RUN(create_dir(cfg->rootfs_path));

    return 0;
}

int create_subdir(struct container_config *cfg, const char *subdir)
{
    char path[PATH_MAX];

    int rc = snprintf(path, sizeof(path), "%s/%s", cfg->rootfs_path, subdir) ;
    if (rc < 0 || rc == 0 || rc >= sizeof(path)) {
        LOG_ERR("create_subdir: snprintf %d failed for %s", rc, cfg->name);
        return -1;
    }

    RUN(create_dir(path));

    return 0;
}


int mount_overlayfs(struct container_config *cfg)
{
    RUN(create_subdir(cfg, "lower"));
    RUN(create_subdir(cfg, "upper"));
    RUN(create_subdir(cfg, "work"));
    RUN(create_subdir(cfg, "merged"));

	int rc;
	char *opts, *merged;

	opts = NULL;
	rc = asprintf(&opts,
		"lowerdir=%s/lower,upperdir=%s/upper,workdir=%s/work", 
		cfg->rootfs_path, cfg->rootfs_path, cfg->rootfs_path
	);

	if (rc == -1) {
		LOG_ERR("mount overlayfs gen opts failed");
		goto done;
	}

	merged = NULL;
	rc = asprintf(&merged, "%s/merged", cfg->rootfs_path);
	if (rc == -1) {
		LOG_ERR("mount overlayfs gen merged failed");
		goto done;
	}

	rc = mount("overlay", merged, "overlay", 0, opts);
	if (rc == -1) {
		LOG_ERR("mount overlayfs %s", merged);
	}

done:
	if (opts) free(opts);
	if (merged) free(merged);

    return rc;
}


/*
int mount_cmd(struct container_config *cfg, const char *rootfs_path)
{
	int rc;
	char *dst_path = NULL;

	rc = asprintf(&dst_path,"%s/%s", rootfs_path, cfg->exec_path)
	if (rc == -1) {
		LOG_ERR("mount cmd gendst %s failed", cfg->exec_path);
		goto done;
	}

	rc = create_dirs(dst_path, 0755);
	if (rc != 0) goto done;

    int fd = open(dst_path, O_CREAT | O_WRONLY, 0755);
    if (fd != -1) {
		LOG_ERR("mount cmd touch %s failed", dst_path);
		goto done;
	}
	close(fd); 

    rc = mount(src, dst, NULL, MS_BIND, NULL);
    rc =mount(NULL, dst, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL);

done:
	if (dst_path) free(dst_path);
    
    return 0;
}
*/

int copy_files(struct container_config *cfg)
{
	return 0;
}

int setup_infrastucture(struct launcher_state *state)
{
    // create dirs
    RUN(create_dir(state->netns_dir));
    RUN(create_dir(state->storage_dir));
    RUN(create_dir(state->runtime_dir));

    for (int i = 0; i < state->num_cfg; i++) {
        struct container_config *cfg = &state->configs[i];
        RUN(create_rootfs(cfg, state->storage_dir));
        RUN(mount_overlayfs(cfg));
        RUN(copy_files(cfg));
        RUN(create_netns(cfg, state->netns_dir, state->netns_suffix));
    }

    // all done
    return 0;
}


static int set_cable_name(struct launcher_state *state, struct container_config *cfg)
{
    const char *prefix = state->cable_prefix;
    if (!prefix) prefix = "";

    int rc = snprintf(cfg->veth, sizeof(cfg->veth), "%s%s", prefix, cfg->name);

    if (rc < 0 || rc == 0 || rc >= sizeof(cfg->veth))  {
        LOG_ERR("seT_cable_name: snprintf %d failed", rc);
        return -1;
    }

    return 0;
}

int setup_network(struct container_config *cfg)
{
    RUN_CMD("ip netns exec %s ip link set %s name eth0", cfg->netns_name, cfg->veth);
    RUN_CMD("ip netns exec %s ip link set lo up", cfg->netns_name);
    RUN_CMD("ip netns exec %s ip link set eth0 up", cfg->netns_name);
    RUN_CMD("ip netns exec %s ip addr add %s dev eth0", cfg->netns_name, cfg->ip_addr);

    cfg->need_network = 0;

    return 0;
}

int create_cable(struct launcher_state *state, 
    struct container_config *x, 
    struct container_config *y)
{
    RUN(set_cable_name(state, x));
    RUN(set_cable_name(state, y));

    RUN_CMD("ip link add %s type veth peer name %s", x->veth, y->veth);
    RUN_CMD("ip link set %s netns %s", x->veth, x->netns_name);
    RUN_CMD("ip link set %s netns %s", y->veth, y->netns_name);

    RUN(setup_network(x));
    RUN(setup_network(y));

    log_status("Created veth pair: %s <-> %s", x->veth, y->veth);

    return 0;
}

int do_go_syncs(struct launcher_state *state)
{
    for (int i = 0; i < state->num_cfg; i++) {
        RUN(parent_go_sync(&state->configs[i]));
    }

    return 0;
}

int do_ready_syncs(struct launcher_state *state)
{
    for (int i = 0; i < state->num_cfg; i++) {
        RUN(parent_ready_sync(&state->configs[i]));
    }

    return 0;
}

int start_containers(struct launcher_state *state)
{
    for (int i = 0; i < state->num_cfg; i++) {
        RUN(container_start(&state->configs[i]));
        state->num_run++;
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
        if (pid == -1) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) break;
            LOG_ERR("wait_containers: waitpid failed");
            return -1;
        }
        if (pid == 0) continue;
        if ((cfg = find_child(state, pid)) == NULL) continue;
        // update run state flags
        if (WIFEXITED(status)) {
            cfg->exit_status = WEXITSTATUS(status);
            cfg->run = 0;
        }
        else if (WIFSIGNALED(status)) {
            cfg->signalled = 1;
            cfg->run = 0;
        }
        else if (WIFSTOPPED(status)) 
            cfg->continued = 1;
        else if (WIFCONTINUED(status))
            cfg->continued = 1;
        // update total run state
        if (!cfg->run) {
            state->num_run--;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    struct launcher_state state;
    struct container_config *client, *server;

    if (init_state(&state, argc, argv) != 0) goto cleanup;
    server = add_config(&state, "db", "db/server" "/bin/server", NULL, NULL, "10.0.0.1");
    client = add_config(&state, "client", "client/client", "/bin/client", "10.0.0.1", "10.0.0.2");
    if (!client || !server) goto cleanup;

    if (setup_infrastucture(&state) != 0) goto cleanup;
    if (start_containers(&state) != 0) goto cleanup;

    if (create_cable(&state, client, server) != 0) goto cleanup;
    if (do_go_syncs(&state) != 0) goto cleanup;
    if (do_ready_syncs(&state) != 0) goto cleanup;

    if (wait_containers(&state) != 0) goto cleanup;

cleanup:   
    state_deinit(&state);

    // all done
    return 0;
}
