/*
 * Container
 */

#ifndef _LAU_CHILD_
#define _LAU_CHILD_

struct lau_config {
    char *name;
    char *cmd_path;
    char *exec_path;
    char *exec_args;
    char *ip_addr;
};

struct lau_child {
    struct simple_sig *sig;
    // user config
    char *name;      // container name
    char *cmd_path;  //  location of cmd
    char *exec_path;  // process to lanuch
    char **exec_argv; // command line args
    int exec_argc;
    char *ip_addr;   // ip addr to add to veth
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

struct lau_child *lau_child_create(void);
void lau_child_free(struct lau_child *child);
int lau_child_load_cfg(struct lau_child *child, struct lau_config *cfg);
int lau_child_setup_network(struct lau_child *child);

int lau_child_prep(struct lau_child *child);
int lau_child_switch_netns(struct lau_child *child);
int lau_child_run(struct lau_child *child);
int lau_child_start(void *arg);

// inline helpers
static inline char *child_get_rootfs(struct lau_child *child)
{
    return child->use_subdir ? child->rootfs_path : child->store_dir;
}

#endif
