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
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>

#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/syscall.h> 

#define LOG_ERR(fmt, ...) do { \
    int _errno = errno; \
    fprintf(stderr, \
        "[%s:%d] ERROR: " fmt ": %s (errno: %d)\n", \
        __func__, __LINE__,  ##__VA_ARGS__, \
        strerror(_errno), _errno); \
} while(0)

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
        return 0;
    }

    rc = system(cmd);
    if (rc == -1) {
        LOG_ERR("system(%s) failed", cmd);
        return 0;
    }

	if (!WIFEXITED(rc)) {
        LOG_ERR("cmd (%s) interupted", cmd);
		return 0;
	}

    int ec = WEXITSTATUS(rc);
	if (ec != 0) {
        LOG_ERR("cmd (%s) failed %d", cmd, ec);
		return 0;
	}

	// all done
    return 1;
}


static int set_identity(const char *name)
{
    int rc = sethostname(name, strlen(name));
    if (rc == -1) {
        LOG_ERR("sethostname %s failed", name);
        return 0;
    }

    return 1;
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
        return 0;
    }

    // - create new mount point
    rc = mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL); 
    if (rc == -1) {
        LOG_ERR("bind mount roofs %s failed", rootfs);
        return 0;
    }

    // - change dir to new mount point
    rc = chdir(rootfs);
    if (rc == -1) {
        LOG_ERR("chdir rootfs %s", rootfs);
        return 0;
    }

    // pivot_root to new mount point (stack old_root)
    rc = syscall(SYS_pivot_root, ".", ".");
    if (rc == -1) {
        LOG_ERR("pivot_root failed");
        return 0;
    }

    // - change root dir to new mount  task_struct (root ptr)
    rc = chroot(".");
    if (rc == -1) {
        LOG_ERR("chroot failed");
        return 0;
    }

    // - change curret dir to new root - task_struct (cwd ptr)
    rc = chdir("/");
    if (rc == -1) {
        LOG_ERR("chdir to / failed");
        return 0;
    }

    rc = umount2("/", MNT_DETACH); 
    if (rc == -1) {
        LOG_ERR("unmount2 / failed");
        return 0;
    }

    return 1;
}

static int set_proc(void)
{
    int rc;

    // new PID namespace - create new /proc
    rc = mkdir("/proc", 0755);
    if (rc == -1) {
        LOG_ERR("mkdir /proc failed");
        return 0;
    }

    mount("proc", "/proc", "proc", 0, NULL);
    if (rc == -1) {
        LOG_ERR("mount /proc faild");
        return 0;
    }

    return 1;
}

static int wait_parent(int fd)
{
    char buf;

    // block until parent writes to us
    int rc = read(fd, &buf, 1);
    if (rc < 1)  {
        LOG_ERR("wait parent %d failed", fd);
        return 0;
    }

    return 1;
}

struct container_args {
    char *hostname;   // container name
    char *rootfs;     // For the bind mount and pivot_root()
    char *exec_path;  // process to lanuch
    char **exec_argv; // command line args
    int  wait_fd;     // wait pipe for parent to setup networking/cgroups
};

// note new task_struct
static int child_func(void *arg)
{
    struct container_args *args = arg;

    if (!set_identity(args->hostname)) _exit(1);
    if (!wait_parent(args->wait_fd)) _exit(2);
    if (!set_rootfs(args->hostname)) _exit(3);
    if (!set_proc()) _exit(4);

    // finally exec the program
    execv(args->exec_path, args->exec_argv);
    LOG_ERR("execv %s failed", args->exec_path);
    _exit(5);
}

int main(int argc, char *argv[])
{
    // allocate a protected memory region for child stack
    // - never ever use malloc as child can corrupt it and parents heap
    // - linux stack grows downwards
    int stack_size = 1024 * 1024;
    char *child_stack = mmap(NULL, stack_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0
    );
    if (child_stack == MAP_FAILED) {
        LOG_ERR("mmap stack %u failed", stack_size);
        exit(1);
    }
    child_stack += stack_size; 

    // create a sync pipe
    int sync_fds[2];
    if (pipe(sync_fds) == -1) {
        LOG_ERR("create sync pipe failed");
        exit(2);
    }

    struct container_args args = { 
        .hostname = "server",
        .rootfs = "/tmp/root",
        .exec_path = "/bin/server",
        .exec_argv = NULL,
        .wait_fd = sync_fds[0]
    };

    // launch child 
    int clone_flags = SIGCHLD | CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET;
    pid_t child_pid = clone(child_func, child_stack, clone_flags, &args);
    if (child_pid == -1) {
        LOG_ERR("clone child(%s,%s) failed", args.hostname, args.exec_path);
        exit(1);
    }

    // close our read end (child still has a copy)
    close(sync_fds[0]);

    // exex all cmds
    int all_done = 0;
    do {
        if (!run_cmd("ip link add veth0 type veth peer name veth1")) break;
        if (!run_cmd("ip link set veth1 netns %d", child_pid)) break;
        // tell client safe to process
        if (write(sync_fds[1], "+", 1) != 1) {
            LOG_ERR("write sync_fds failed"); 
            break;
        }
        all_done = 1;
    } while(0);

    if (!all_done) {
        // run_cmd or write_sync failed ?
        kill(child_pid, SIGKILL);
    }

    close(sync_fds[1]);

    // wait for child
    int status;
    do {
        int rc = waitpid(child_pid, &status, 0);
        if (rc == -1) {
            LOG_ERR("waitpid (%u) failed", child_pid);
            exit(5);
        }
        if (WIFEXITED(status))
           printf("exited, status=%d\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
           printf("killed by signal %d\n", WTERMSIG(status));
        else if (WIFSTOPPED(status))
           printf("stopped by signal %d\n", WSTOPSIG(status));
        else if (WIFCONTINUED(status))
           printf("continued\n");
    } while (!WIFEXITED(status) && !WIFSIGNALED(status));

    // child has exited okay
    return 0;
}


