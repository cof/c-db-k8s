#!/usr/bin/gawk -f

BEGIN {
    # default permissions
    syscalls["execve"] = 1
    syscalls["exit_group"] = 1
    syscalls["rt_sigreturn"] = 1
    syscalls["brk"] = 1
    syscalls["getpid"] = 1
}

# add strace syscall entry
{
    name = $NF
    if (name ~ /^[a-z_]/ && name != "total" && name != "syscall") {
        syscalls[name] = 1
    }
}

#  output BPF syscall list
END {
    # build/sort list of syscall names
    count = 0
    for (name in syscalls) {
        syscalls[++count] = name
    }
    asort(names)

    #  gen list of allowed syscalls
    for (i = 1; i <= count; i++) {
        offset = count - i + 1
        printf "BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_%s, %d, 0),\n", names[i], offset
    }

    if (count > 1) {
        print "BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),"
        print "BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)"
    }
}
