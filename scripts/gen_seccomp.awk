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
        names[++count] = name
    }
    asort(names)

    # indent 8 spaces
    n = 8

    # header
    printf "%*s%s\n", n, "", "// whitelist syscalls"

    #  add list of allowed syscalls
    for (i = 1; i <= count; i++) {
        offset = count - i + 1
        printf "%*sBPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_%s, %d, 0),\n", 
            n, "", names[i], offset
    }

    printf "%*s%s\n", n, "", "// actions"
    if (count > 1) {
        printf "%*s%s\n", n, "", "BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),"
        printf "%*s%s\n", n, "", "BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)"
    }
}
