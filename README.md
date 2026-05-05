# db-k8s

A research project into how Linux containers work.

This project began as a 4-day "impossible sprint" to implement a full-stack container runtime and DNS subsystem from first principles with just plain old vim,tmux and gcc. It has since evolved into a research platform for exploring how Linux containers work.

There are two parts to this project. 

- **Container Launcher** - Custom application isolation using Linux namespaces and OverlayFS.
- **Kubernetes** - Automated application deployment using Docker, k3d and k8s pods.

## Prerequisites

- **GCC**: Version 9.0 or higher.
- **make**: Version 4.1 or higher.
- **Bash**: Version 4.0+ for running tests
- **awk**: Version 4.0+ for running tests
- **Docker**: for building container images
- **k3d**:  for creating clusters, loading docker images and enforcing network policy
- **kubectl**: for managing pods

## Building the Project

- **make all** (Default): Compiles server,client,launcher
- **make deploy**  Deploy client and server into k8s pods
- **make test-full** Run all tests (cmds,launcher,k8s)
- **make spotless**: wipe complied binaries, VMS, k8s pods

## Design

- All application code was written in C with no 3rd party libs.
- Uses embeddable structures and avoid malloc where possible
- VM Provisioning : fully automated VM provisioning via build_vm.mk
- K8s Integration: automated Kubernetes provisioning and testing.
- LOG : levels-based logging subsystem (FATAL, ERROR, INFO, DEBUG)
- MACROS : macros for safe pointer and offset calculation
- STR_UTIL : custom string buffer and string slice API
- UTIL :  helpers for signal handling, system execution and command-line parsing
- HASHMAP : type-safe api uses C11 generics,X-Macros,Fibonacci hashing,open addressing
- RWBUF   : memory buffer api
- DNS-PROTO  : rfc1035 compliant codec
- DNS-RESOLV : DNS subsystem (getaddrinfo replacement)
- SOCK : socket layer api supporting non-blocking client/servers/buffering
- DB   : key/value store api supporting a pure memory or mmap database file

### DNS-RESOLV

DNS-RESOLV is a complete DNS subsystem designed to be a getaddrinfo replacement.

- port name resolution using /etc/service
- hostname resolution using /etc/hosts 
- resolver configuration using /etc/resolv.conf 
- nameserver management supportng attempts/timeout/ndots/search
- Hyrbid UDP and TCP support
- Parallel AAAA and A query support
- Truncation (TC) support for UDP fall back to TCP
- ESDN0 supporting UDP packet sizes > 512 bytes
- Uses DNS-PROTO a full rfc1035 compliant codec (no mallocs).
- DNS cache with TTL support
- non-blocking I/O using poll

### VM Provisioning

- A container launcher that modifies its host namespaces **needs a VM** to safely test
- Makefile uses build_vm.mk to provision a VM called `test-lau` based on Alpine Linux
- Simply run make test-lau to download, create, install and run VM
- Uses cloud-config user-data.yaml template file to configure VM
- Injects SSH public key (id_rsa.pub) into user-data.yaml file
- VM can be access via ssh key or alpine:alpine or console root:alpine
- alpine user is setup as sudo user (using doas)
- make test-lau simply installs client,server,launcher into VM and runs launcher
- ssh directly to VM with ssh alpine@test-lau if nsswitch.conf allows it
- Add libvirt_guest to hosts line in /etc/nsswitch.conf e.g "hosts: files libvirt_guest"

## 1. Container Launcher

A custom container launcher that runs applications inside isolated namespaces.

Code consist of a container launcher and database client and server to test it.
The database client and server are reused by k8s.

- **launcher** A Linux container runtime management tool
- **server**  key-value DB server supporting cli SET,GET,DEL operations
- **client**  telnet-style DB client using a 4-way TCP-wrapper pipe


## 1.1 Launcher

A custom linux container launcher for running applications inside isolated namespaces.

**Features**

- Create and manages its own network namespaces
- Creates an isolated filesystem for each container
- OverLay FS support using a provided rootfs-dir
- Supports privilege dropping (sudo,caps,privs,seccomp)
- Use a simple API for container configuration
- An Alpine Linux VM was used to test launcher

**Design**

- single-threaded application written in C with no 3rd party libs
- create run-time dirs before running containers
- Create a folder for each container to hold its rootfs
- create veth devices for container
- creates a network namespace for each container
- creates a child process for each container
- child switches to its private rootfs
- child creates proc
- child applies security settings
- child execs the client or server binary
- uses mount and clone to create netns and containers

**Testing**


**Example usage**

	$ make test-lau
	[Installing files]
	install -D -m 755 server bin/db/server
	install -D -m 755 client bin/client/client
	install -D -m 755 launcher bin
	[+] Creating VM-DISK: vmdir/myalpine.qcow2
	[+] Installing VM: test-lau
	[+] Started VM: test-lau
	[+] Waiting for VM test-lau to reach SSH
	 ... still waiting (1/30)
	 ... still waiting (2/30)
	 ... still waiting (3/30)
	 ... still waiting (4/30)
	 ... still waiting (5/30)
	 ... still waiting (6/30)
	 ... still waiting (7/30)
	 ... still waiting (8/30)
	 => VM is UP at 192.168.122.199.
	[+] Running test-lau
	 => Copying bin to alpine@192.168.122.199:/home/alpine
	 => Sending cmds to test-lau ...
	[+] Database listening on [::]:6379
	[+] Client connected from 10.0.0.2:39704
	[+] Connectivity test: OK
	> SET foo bar
	OK
	> GET foo
	bar
	> DEL foo
	OK
	> QUIT
	OK
	[+] Connection closed by server
	[+] server PID:1 shutting down: got signal 15 (Terminated) from UID:0 PID:0 
	 => Fetching logs
	 => TEST tests/test_req.txt [ PASS ]
	 => TEST tests/test_rsp.txt [ PASS ]
	 => Ran 2 tests: 2 passed, 0 failed (100% success)
	✅ test-lau complete.

    c-db-k8s$ ssh alpine@test-lau
    launcher:~$ ll
    total 2
    drwxr-xr-x    4 alpine   alpine        1024 Mar 29 16:51 bin
    drwxr-sr-x    5 root     alpine        1024 Mar 29 16:51 test-lau
    launcher:~$ cd bin/
    launcher:~/bin$ tree
    .
    ├── client
    │   └── client
    ├── db
    │   └── server
    └── launcher

    2 directories, 3 files
    launcher:~/bin$ sudo ./launcher 
    [+] Created network namespace: db-ns
    [+] Created network namespace: client-ns
    [+] Created veth pair: veth-client <-> veth-db
    [+] Database listening on [::]:6379
    [+] Client connected from 10.0.0.2:49288
    [+] Connectivity test: OK
    > set foo bar
    OK
    > get foo
    bar
    > set foo two
    OK
    > get foo
    two
    > del foo
    OK
    > get foo
    FAIL
    > quit
    [+] OK
    server-close 10.0.0.2:49288
    > [+] Connection closed by server
    [+] Container 'client' exit ok (pid=2502 why=exit 0)
    [+] server PID:1 shutting down: got signal 15 (Terminated) from UID:0 PID:0 


### 1.2 Server

A database (DB) server than supports a telnet-style CLI to access a key/value store.  
Client's simply connect to the server and send plain-text commands to modify the store.

**Supported Commands:**

- SET key value - store a value for given key (e.g set username joe)
- GET key       - retrieve value for given  key
- DEL key       - delete key/value from store
- QUIT          - close connection

**Features**

- cmd-line allows port selection, database mode and logging levels
- telnet style CLI
- DB backend can use  crash proof database file (mmap)
- easily scales well beyond 100k+ connections using an mmap DB and event-driven I/O

**Design**

- single-threaded application written in C with no 3rd party libs
- Uses epoll (level triggered) to monitor all socket events
- Uses SOCK-API to create non-blocking dual-stack (IPv4|6) sockets
- SOCK API uses DNS-RESOLV as a getaddrinfo replacement
- SOCK API uses RWBUF api to read and write lines to sockets
- DB API used to update key/value store
- DB backend can support either in memory store or mmap database file
- LOG api used to capture errors and logs them to stderr
- listens by default using wildcard [::]:6379

**Example usage**

    $ ./server 
    [+] Database listening on [::]:6379

	$ ./server --help
	Usage: server [OPTIONS]

	Options:
     
	 --help      This help
	 --hostname  hostname to listen on
	 --port      port to listen on (default=6379)
	 --database  Path to database file
	 --log-line  log request and response lines
	 --log-level logging level  (default=3)
	 --argv      Dump argv to stdout

	Examples:
	  server --log-level 4
	  server --hostname 127.0.0.1 --port 6379 --database mydb.bin

### 1.3 Client

A telnet client that connect to a server address.  
Client simply reads and writes lines between stdio and server socket.

**Features**

- cmd-line allows server address/port selection and logging levels
- Supports normal stdin/stdout or pipes

**Design**

- single-threaded application written in C with no 3rd party libs
- Uses a 4 way TCP wrapper pipe to exchange lines between stdio/socket
- Reads lines from stdin and writes then to socket
- Reads lines from socket  and writes then to stdout
- Uses SOCK api to create socket and manage stdin,stdout fds
- Uses poll() to monitor fd activity (stdin,stdout,socket)
- LOG api used to capture errors and logs them to stderr

**Example usage**

    $ ./client --hostname 127.0.0.1
    [+] Connectivity test: OK
    > set foo bar
    OK
    > get foo
    bar
    > del foo
    OK
    > quit
    OK
    > 
    [+] Connection closed by foreign host.

	$ ./client --help
	Usage: client [OPTIONS]
     
	Options:
     
	 --help      This help
	 --hostname  hostname to connect to
	 --port      port to listen on (default=6379)
	 --log-line  log request and response lines
	 --log-level logging level (default=3)
	 --argv      Dump argv to stdout

	Examples:
	  client --hostname db-service
	  client --hostname localhost --port 6379
	  client --hostname localhost --log-line


## 2. Kubernetes

Deploy the client and server applications on Kubernetes.
    
For first run simply do

    $ make test-k8s

This will:

- Compile the client,server binaries
- Install them into bin folder
- Build docker images files
- Create a k3d cluster
- Load docker images into cluster
- Start/apply the pods
- Run k8s tests

**Example usage**

    $ make test-k8s
    [+] Running wait-pods
     => kubectl wait for db-pod ready ...
     => pod/server-pod-0 condition met
     => kubectl wait for client-pod ready ...
     => pod/client-app-9c5c88dff-cbtwl condition met
     => pod/client-app-9c5c88dff-dg7xk condition met
     => pod/client-app-9c5c88dff-lf9qw condition met
     => Wait for 1 client-pod connected ...
     => ✓ wait-pods complete.
    [+] Running test-pod
     => Using client-pod: pod/client-app-9c5c88dff-cbtwl
     => Sending cmds
     => Fetching logs
     => Checking results
     => check SET test-pod 3OUMYh07ASwejVqzuFlENbf3zM4sJK [ PASS ]
     => check GET test-pod [ PASS ]
     => check DEL test-pod [ PASS ]
     => Ran 3 tests: 3 passed, 0 failed (100% success)
    [+] Running test-net
     => db-pod     -> internet        ✗ DENIED  [ PASS ]
     => client-pod -> internet        ✗ DENIED  [ PASS ]
     => client-pod -> db-pod:6379     ✓ ALLOWED [ PASS ]
     => random-pod -> db-pod:6379     ✗ DENIED  [ PASS ]
     => Ran 4 tests: 4 passed, 0 failed (100% success)
    ✅ test-k8s complete.


**Features**

- Database deployment: StatefulSet, PersistentVolumeClaim, DNS Service, Limits
- Client deployment: configurable replicas, DNS connections, readiness probe
- Network policy enforcement : client pods can only reach database
- Observability: client logs reqs/rsps, server logs connections
- Automated provisioning: make deploy
- Automated test-suite: make test-k8s 
- Status checks: make list-pod|list-net|show-log

**Example usage**

To check the pods are up:

    $ make list-pod
    kubectl get statefulset/server-pod 
    NAME         READY   AGE
    server-pod   1/1     4m2s
    kubectl get deployment/client-app
    NAME         READY   UP-TO-DATE   AVAILABLE   AGE
    client-app   3/3     3            3           4m2s
    kubectl get pods -l 'app in (client-pod, server-db)'
    NAME                         READY   STATUS    RESTARTS   AGE
    client-app-644b4d99d-hfh8w   1/1     Running   0          3m35s
    client-app-644b4d99d-tg9xs   1/1     Running   0          4m2s
    client-app-644b4d99d-zzl27   1/1     Running   0          3m40s

## 3. Testing

Testing client/server/launcher and k8s requires a large test-suite.

The makefile has the following test targets.

- **make test**  run basic tests
- **make test-full** run all tests (test-cmds,test-lau,test-k8s)   
&nbsp;
- **make test-cmds** run client,server tests
- **make test-lau** run ./launcher tests (using VM)
- **make test-k8s** run k8s tests (wait-pods, test-pod,test-net)   
&nbsp;
- **make test-server** run ./server tests
- **make test-client** run ./client tests
- **make wait-pods** wait for pods to come up
- **make test-pod** run pod GET|SET|DEL tests
- **make test-net** run network policy tests
