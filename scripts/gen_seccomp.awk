#!/usr/bin/gawk -f

BEGIN {
    # Force these into 'seen' so the loop below won't add them again
    seen["execve"] = 1
    seen["prctl"] = 1
    seen["exit_group"] = 1
    seen["rt_sigreturn"] = 1
    seen["brk"] = 1
}

# 1. Capture and deduplicate from the log
{
    name = $NF
    if (name ~ /^[a-z_]/ && name != "total" && name != "syscall") {
        seen[name] = 1
    }
}

# 2. Sort and Generate
END {
    count = 0
    for (name in seen) {
        unique_names[++count] = name
    }

    asort(unique_names)

    for (i = 1; i <= count; i++) {
        # THE FIX: offset = (total_remaining_lines) + 1 
        # This skips the remaining list AND the 'KILL' statement to land on 'ALLOW'
        offset = count - i + 1
        printf "    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_%s, %d, 0),\n", unique_names[i], offset
    }
}
