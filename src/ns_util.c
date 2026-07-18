/* SPDX-License-Identifier: MIT | (c) 2026 [cof] */

/*
 * NameSpace Util API for containers
 * ---------------------------------
 * An api to manage namepspaces
 * See ns_uti.h for description.
 * --------------------------------
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
 * namespace : child process namespace changes
 * security  : child process security changes
 * helpers   : status check, close func
 *
 * Refs:
 * -----
 * man 7 namespaces
 * man 2 clone
 * man 2 pivot_root
 * man 2 wait
 * man 2 seccomp
 * Kerrisk - TLPI - The Linux Programming Interface
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
#include "hashmap.h"
#include "ns_util.h"

// terminate child process pid, wait usecs
void shutdown_pid(int pid, int wait)
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

// convert cmd-line exec_args str into argv array
char **exec_args_parse(const char *exec_path, const char *exec_args, int *argc)
{
    wordexp_t p = { 0 };

    if (exec_args && wordexp(exec_args, &p, WRDE_NOCMD) != 0) {
        if (argc) *argc = 0;
        return log_error_rn("wordexp failed");
    }

    char **argv = malloc((p.we_wordc + 2) * sizeof(char *));
    if (!argv) return log_errno_rn("malloc failed");

    argv[0] = strdup(exec_path);
    if (!argv[0]) {
        free(argv);
        return log_errno_rn("strdup failed");
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

// generate an id str based on hash of name
char *gen_id(char *buf, int len, const char *name)
{
    uint32_t id = dbj2a_hash_str(name) ^ (uint64_t) time(NULL);

    int rc = snprintf(buf, len, "%08x", id);
    if (rc < 0 || rc == 0 || rc >= len)  {
        return log_error_rn("gen_id failed for %s", name);
    }

    return buf;
}

// generate a path string
char *gen_path(const char *dir, const char *name)
{
    if (!dir || !name) return NULL;

    if (*name == '/') name++;

    char *path = NULL;
    int rc = asprintf(&path, "%s/%s", dir, name);

    if (rc == -1) {
        // out of memory ?
        return log_errno_rn("gen_path failed for %s", name);
    }

    return path;
}

// parent|child close its pipe end
int sync_pipe_close(int *rd_fd, int *wr_fd,
    const char *who, const char *what, const char *name,
    pid_t pid)
{
    if (close_fd(rd_fd) != 0) {
        return log_errno_rf("%s pipe-close rd %s for child %s pid %d failed", who, what, name, pid);
    }

    if (close_fd(wr_fd) != 0) {
        return log_errno_rf("%s pipe-close wr %s for child %s pid %d failed", who, what, name, pid);
    }

    return 0;
}

// parent|child process reads from its pipe end
int sync_pipe_read(int *fd,
    struct simple_sig *sig,
    const char *who, const char *what, const char *name,
    pid_t pid)
{
    log_debug("%s pipe-read %s (name=%s pid=%d)", who, what, name, pid);

    // wait for peer to write
    ssize_t nr;
    char ch;
    while ((nr = read(*fd, &ch, 1)) == -1) {
        if (errno == EINTR) {
            if (!sig->run) return LAU_INTR;
            continue;
        }
        return log_errno_rf("%s pipe-read %s failed for %s pid %d", who, what, name, pid);
    }

    // read done - close our end
    if (close_fd(fd)) {
        return log_errno_rf("%s pipe-close %s failed for %s pid %d", who, what, name, pid);
    }
    // peer gone ?
    if (nr == 0) {
        return log_error_rc(LAU_EOF, "%s pipe-read %s eof for %s pid %d", who, what, name, pid);
    }

    log_debug("%s pipe-read done %s (name=%s pid=%d)", who, what, name, pid);

    return 0;
}

// parent|child process writes to its pipe end
int sync_pipe_write(int *fd,
    struct simple_sig *sig,
    const char *who, const char *what, const char *name,
    pid_t pid)
{
    log_debug("%s pipe-write %s (name=%s pid=%d)", who, what, name, pid);

    // wake up peer
    while (write(*fd, "!", 1) == -1)  {
        if (errno == EINTR) {
            if (!sig->run) return LAU_INTR;
            continue;
        }
        int ec = errno = EPIPE ? LAU_PIPE : LAU_ERR;
        return log_errno_rc(ec, "%s pipe-write %s  failed for %s", who, what, name);
    }

    // write-done - close our end
    if (close_fd(fd)) {
        return log_errno_rf("%s pipe-close %s failed for %s pid %d", who, what, name, pid);
    }

    log_debug("%s pipe-write done %s (name=%s pid=%d)", who, what, name, pid);

    // all done
    return 0;
}

// create a folder
int create_dir(const char *path, mode_t mode, bool can_exist)
{
    int rc = mkdir(path, mode);

    if (rc == -1 && (!can_exist || errno != EEXIST)) {
        return log_errno_rf("create_dir %s failed", path);
    }

    return 0;
}

// aka mkdir -p - modifies path
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

// aka mkdir -p
int create_path(const char *path, mode_t mode)
{
    // dupe path as we need to modify it
    char *tmp = strdup(path);
    if (!tmp) return log_errno_rf("create_path strdup %s failed", path);

    int rc = create_path_nocopy(tmp, mode);
    free(tmp);

    return rc;
}

int create_path_for_file(const char *file, mode_t mode)
{
    if (!file) return log_error_rf("file name is null");

    char *tmp = strdup(file);
    if (!tmp) return log_errno_rf("strdup %s failed", file);

    // chop the file name from, path
    char *ptr = strrchr(tmp, '/');
    if (!ptr) {
        free(tmp);
        return log_error_rf("Not a file name %s", file);
    }
    *ptr = '\0';

    // create file path
    int rc = create_path_nocopy(tmp, mode);
    free(tmp);

    return rc;
}

// create new sub-folder in dir
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

// copy file - uses sendfile() for fast copy
int copy_file(const char *src, const char *dst)
{
    int rc = -1;

    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) {
        return log_errno_rf("copy_file open src %s failed", src);
    }

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

// open host network namespace
int open_host_netns(void)
{
    // fd must be for this process only (O_CLOEXEC)
    int fd = open(HOST_NETNS_PATH, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        return log_errno_rf("open host_netns %s failed", HOST_NETNS_PATH);
    }

    return fd;
}

// set netns back to host network namespace
int restore_host_netns(int netns_fd)
{
    int rc = setns(netns_fd, CLONE_NEWNET);
    if (rc) {
        return log_errno_rf("restore netns %s failed", HOST_NETNS_PATH);
    }

    return 0;
}

// switch to a child netns
int switch_child_netns(int *fd, const char *name)
{
    int rc = setns(*fd, CLONE_NEWNET);
    if (rc) return log_errno_rf("switch setns %d for %s failed", *fd, name);

    rc = close_fd(fd);
    if (rc) return log_errno_rf("close netns for child %s failed", name);

    return 0;
}

// create an file for a netns
int create_netns_file(const char *netns_path)
{
    int fd = open(netns_path, O_RDONLY | O_CREAT | O_EXCL, 0600);

    if (fd == -1) {
        // error ?
        if (errno != EEXIST) {
            return log_errno_rf("open %s failed", netns_path);
        }
        // file already exists - possible crash ?
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

// mount an OverlayFS
int mount_overlay(const char *path,
    const char *lowerdir, const char *upperdir,
    const char *workdir,
    const char *who)
{
    char *opts_str = NULL;
    int rc = asprintf(&opts_str,
        "lowerdir=%s,upperdir=%s,workdir=%s",
        lowerdir, upperdir, workdir
    );
    if (rc == -1) {
        return log_errno_rf("mount overlay genopts failed for %s", who);
    }

    rc = mount("overlay", path, "overlay", 0, opts_str);
    free(opts_str);

    if (rc == -1) {
        return log_errno_rf("mount overlay %s for %s failed", path, who);
    }

    return 0;
}

int unmount_overlay(const char *path, const char *name)
{
    int rc = umount2(path, MNT_DETACH);
    if (rc < 0 && errno != EINVAL) {
        return log_errno_rf("unmount overlay %s for %s failed", path, name);
    }

    return 0;
}

// mount roofs_dir into container root
int mount_rootfs(const char *rootfs_dir, const char *rootfs_path)
{
    int rc = mount(rootfs_dir, rootfs_path, NULL, MS_BIND, NULL);
    if (rc != 0) {
        return log_errno_rf("mount bind rootfs %s to %s failed", rootfs_dir, rootfs_path);
    }

    // make all further mounts private
    rc = mount(NULL, rootfs_path, NULL, MS_REC | MS_PRIVATE, NULL);
    if (rc) {
        int _errno = errno;
        umount2(rootfs_path, MNT_DETACH);
        return log_ec_rf(_errno, "mount private %s failed", rootfs_path);
    }

    return 0;
}

// mount a host binary into container filesystem - no file copy
int mount_cmd(const char *host_path, const char *rootfs_path)
{
    log_debug("mount_cmd (host=%s, rootfs=%s)", host_path, rootfs_path);

    // create an empty file - touch rootfs_path
    int fd = open(rootfs_path, O_CREAT | O_WRONLY, 0755);
    if (fd == -1) {
        return log_errno_rf("mount_cmd open(%s) failed", rootfs_path);
    }
    close(fd);

    // create a writable bind-mount
    if (mount(host_path, rootfs_path, NULL, MS_BIND, NULL) < 0) {
        // failed - unlink
        int _errno = errno;
        unlink(rootfs_path);
        errno = _errno;
        return log_errno_rf("mount_cmd bind mount %s failed", rootfs_path);
    }

    // update bind-mount to read-only
    if (mount(NULL, rootfs_path, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0) {
        // failed - unmount|unlink
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

// make file a mount point
int mount_file(const char *path)
{
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

// bind mount a new netns file - uses fork/exec to safely bind mount
int mount_netns(const char *netns_path)
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

    // fork a child process to do the bind mount
    pid_t pid = fork();
    if (pid < 0) {
        // fork failed
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

    // parent process
    int status;
    pid_t p = waitpid(pid, &status, 0);
    if (p == -1) {
        // waitpid failed ?
        int _errno = errno;
        umount2(netns_path, MNT_DETACH);
        unlink(netns_path);
        errno = _errno;
        return log_errno_rf("waitpid failed");
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        // child failed
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

// create a new veth device
int veth_add(const char *veth, const char *peer)
{
    char tmp[128];
    struct sbuf buf = SBUF_INIT(tmp, sizeof(tmp));

    return run_cmd(&buf, RUN_NULL, "ip link add %s type veth peer name %s", veth, peer);
}

// delete veth device
int veth_del(const char *veth)
{
    char tmp[128];
    struct sbuf buf = SBUF_INIT(tmp, sizeof(tmp));

    return run_cmd(&buf, RUN_NULL, "ip link del %s", veth);
}

// set netns for veth
int veth_setns(const char *veth, const char *netns)
{
    char tmp[128];
    struct sbuf buf = SBUF_INIT(tmp, sizeof(tmp));

    return run_cmd(&buf, 0, "ip link set %s netns %s", veth, netns);
}

// generate id-str for veth
static int veth_gen_idstr(const char *name,
    char *veth, int veth_len,
    char *peer, size_t peer_len)
{
    uint32_t id = dbj2a_hash_str(name) & 0xffffffff;
    int salt = 10;

    do {
        int rc = gen_str(veth, veth_len, "vth%08x%d", id, salt);
        if (rc) return rc;
        rc = gen_str(peer, peer_len, "%s", veth);
        if (rc) return rc;
        rc = veth_add(veth, peer);
        if (rc == 0) return 0;
    } while(--salt);

    // give up
    return -EEXIST;
}

// create and setup a veth for container netns
int veth_setup(const char *cont_name, const char *netns)
{
    char tmp[128];
    struct sbuf buf = SBUF_INIT(tmp, sizeof(tmp));

    char veth[IFNAMSIZ];
    char peer[IFNAMSIZ];

    RUN(veth_gen_idstr(cont_name, veth, sizeof(veth), peer, sizeof(peer)));

    if (run_cmd(&buf, 0, "ip link set %s netns %s", peer, netns)) return -1;
    if (run_cmd(&buf, 0, "ip link set %s up", veth)) return -1;

    log_info("+", "Created veth %s", veth);

    return 0;
}

/* namespace : child process namespace changes */

// set container hostname
int set_identity(const char *name)
{
    int rc = sethostname(name, strlen(name));
    if (rc == -1) return log_errno_rf("sethostname %s failed", name);

    return 0;
}

/*
 * set_rootfs - switch child process to new root file system
 *
 * 7 steps:
 * ========
 * 1 - make mount space private
 * 2 - create new mount point
 * 3 - change dir to new mount point
 * 4 - pivot_root to new mount point (stack old_root)
 * 5 - change root dir to new mount
 * 6 - change curret dir to new root
 * 7 - umount/remove old root
 *
 * Refs:
 * - man 2 pivot_root
 */
int set_rootfs(const char *rootfs)
{
    // 1 - make mount space private
    int rc = mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    if (rc) return log_errno_rf("private mount failed");

    // 2 - create new mount point
    rc = mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL);
    if (rc) return log_errno_rf("bind mount roofs %s failed", rootfs);

    // 3 - change dir to new mount point
    rc = chdir(rootfs);
    if (rc) return log_errno_rf("chdir rootfs %s", rootfs);

    // 4 - pivot_root to new mount point (stack old_root)
    rc = syscall(SYS_pivot_root, ".", ".");
    if (rc) return log_errno_rf("pivot_root failed");

    // 5 - change root dir to new mount  task_struct (root ptr)
    rc = chroot(".");
    if (rc) return log_errno_rf("chroot failed");

    // 6 - change curret dir to new root - task_struct (cwd ptr)
    rc = chdir("/");
    if (rc) return log_errno_rf("chdir to / failed");

    // 7 - remove/discard old root
    rc = umount2("/", MNT_DETACH);
    if (rc) return log_errno_rf("unmount2 / failed");

    // all done
    return 0;
}

// child process - create proc dir
int set_proc(void)
{
    // new PID namespace - create new /proc
    if (create_dir("/proc", 0755, 1) != 0) {
        return log_errno_rf("mkdir /proc failed");
    }

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        return log_errno_rf("mount /proc failed");
    }

    return 0;
}

// child process - bring up container network - lo, eth0 and ip addr
int create_network(const char *veth_name, const char *ip_addr)
{
    char tmp[128];
    struct sbuf buf = SBUF_INIT(tmp, sizeof(tmp));

    if (run_cmd(&buf, 0, "ip link set %s name eth0", veth_name)) return -1;
    if (run_cmd(&buf, 0, "ip addr add %s dev eth0", ip_addr)) return -1;
    if (run_cmd(&buf, 0, "ip link set lo up")) return -1;
    if (run_cmd(&buf, 0, "ip link set eth0 up")) return -1;

    return 0;
}

/* security  : child process security changes */

// child - drop all capabilities
int drop_bounding_set(const char *name)
{
    for (int i = 0; i <= 63; i++) {
        int rc = prctl(PR_CAPBSET_DROP, i, 0, 0, 0);
        if (rc == -1) {
            if (errno == EINVAL) break;
            if (errno == EPERM) {
                return log_errno_rf("drop-boundin_set failed for container %s", name);
            }
        }
    }

    return 0;
}

// child - clear existing capabilities
int clear_all_caps(const char *name)
{
    struct __user_cap_header_struct header = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct data[2] = { {0} };

    int rc = syscall(SYS_capset, &header, data);
    if (rc != 0) {
        return log_errno_rf("clear_all_caps failed for container %s", name);
    }

    return 0;
}

// child - drop sudo
int drop_sudo(const char *name, uid_t uid, uid_t gid)
{
    /* needs musl-gcc or dynanic libs
    if (initgroups(cnt->user_name, cnt->gid) != 0) {
        return log_errno("initgroups %s failed", cnt->user_name);
    }
    */

    if (setgid(gid) != 0) {
        return log_errno_rf("setgid %u failed for %s", gid, name);
    }

    if (setuid(uid) != 0) {
        return log_errno_rf("setuid %u failed for %s", uid, name);
    }

    return 0;
}

// child - drop right to new privileges
int drop_new_privs(const char *name)
{
    int rc = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    if (rc != 0) {
        return log_errno_rf("prctl set-nonnew_privs failed for container %s", name);
    }

    return 0;
}

/*
 * Apply syscall security filters
 *
 * TODO add predefined lists
 *
 * Refs:
 * - man 2 seccomp and code example
 * - linux.git samples/seccomp/bpf-direct.c
 * - make gen-seccomp - build/seccomp_rules.h
 */
int apply_seccomp(const char *name)
{
    struct sock_filter filter[] = {

        //  Arch check (Required for security to prevent 32-bit bypass)
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (offsetof(struct seccomp_data, arch))),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64 , 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

        //  Load syscall number
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (offsetof(struct seccomp_data, nr))),

        // whitelist syscalls
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_accept4, 33, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_arch_prctl, 32, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_bind, 31, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_brk, 30, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_close, 29, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_connect, 28, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_epoll_create1, 27, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_epoll_ctl, 26, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_epoll_wait, 25, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_execve, 24, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_exit_group, 23, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fcntl, 22, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getpid, 21, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getrandom, 20, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_listen, 19, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mprotect, 18, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_newfstatat, 17, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_openat, 16, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_poll, 15, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_prlimit64, 14, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_read, 13, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_readlink, 12, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_rseq, 11, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_rt_sigaction, 10, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_rt_sigreturn, 9, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_set_robust_list, 8, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_set_tid_address, 7, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_setsockopt, 6, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_shutdown, 5, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 4, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_uname, 3, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_write, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_writev, 1, 0),
        // actions
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

    };

    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter)/sizeof(filter[0])),
        .filter = filter,
    };

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != -0) {
        return log_errno_rf("prctl SET_SECCOM failed for container %s", name);
    }

    return 0;
}
