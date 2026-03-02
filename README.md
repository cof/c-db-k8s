Toos:
    server
    client
    launcher

 // Drop CAP_SYS_ADMIN, CAP_NET_RAW, etc.
 Drops capabilitiesi - libcap-dev
 Uses seccomp filter to restrict syscallsi - libseccomp-dev 

 apk add libcap-dev libseccomp-dev

Requirements
- gcc 
- libguestfs - virt-install
