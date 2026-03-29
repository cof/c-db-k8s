# db-k8s

Run a database (DB) client and server application inside containers.

There are 2 parts to this project. 

- **Container**  Containerized DB Client/Server with Custom Launcher
- **Kubernetes**  Deploy DB client/server inside k8s pods

## Design Notes

- Application code written in C with no 3rd party libs
- Uses custom API's (UTIL,LOG,RWBUF,SOCK)
- Makefile has large test-suite for launcher and k8s testing

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


## 1. Container

Runs a database (DB) client and server inside containers using custom launcher.

- **launcher** A Linux container runtime management tool
- **server**  key-value DB server supporting cli SET,GET,DEL operations
- **client**  telnet-style DB client using a 4-way TCP-wrapper pipe

## 1.1 Launcher

A custom linux container launcher for running applications inside isolated namespaces.

**Supported features**

- Create and manages its own network namespaces
- Creates an isolated filesystem for each container
- OverLay FS support using a provided rootfs-dir
- Supports privilege dropping (sudo,caps,privs,seccomp)
- Use a simple API for container configuration
- An Alpine Linux VM was used to test launcher

**Design**

- single-threaded applicaton written in C with no 3rd party libs
- create run-time dirs before running containers
- Create a folder for each container to hold its rootfs
- create veth devices for container
- creates a network namespace for each container
- creates a child process for each container
- child switches to its private rootfs
- child creates proc
- child applys security settings
- child execs the client or server binary
- uses mount and clone to create netns and containers

**Testing**

- A launcher that modifies its host namespaces **needs** a VM to safely test
- Makefile supports provisioning a test VM called test-lau based on Alpine Linux 
- Simply run make test-lau to download, create, install and run VM
- Uses cloud-config user-data.yaml template file to configure VM
- Embeds users public key (id_rsa.pub) into user-data.yaml file
- VM can be access via ssh key or alpine:alpine or console root:alpine
- alpine user is setup as sudo user (using doas)
- make test-lau simply installs client,server,launcher into VM and runs launcher
- You can ssh directly to VM with ssh alpine@test-lau if nsswitch.conf allows it
- Just edit /etc/nsswitch.conf and add libvirt_guest to hosts line e.g "hosts: files libvirt_guest" 

**Example usage**

    $ make test-lau
      CC    build/ns_util.o
      CC    build/lau_child.o
      CC    build/launcher.o
      LD    launcher
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
     => VM is UP at 192.168.122.224.
    [+] Running test-lau
     => Copying bin to alpine@192.168.122.224:/home/alpine
     => Running /home/alpine/bin/launcher ...
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

A database (DB) server than supports a telnet-style cli to accesss a key/value store.  
Client's simply connect to server  and send commands to modify key/value store.

**Supported Commands:**

- SET key value - store a key value
- GET key       - retrive a key value
- DEL key       - delete key/value from store
- QUIT          - close connection

**Supported featues**

- telnet style cli SET|GET|DEL  cmds
- DB backend use mmap to provide crash proof database file
- Uses Multiplexing and non-blocking I/O for all read/writes
- scales easily well beyond 100k+ connections using an mmap DB and event-driven I/O

**Design**

- single-threaded applicaton written in C with no 3rd party libs
- Uses epoll (level triggered) to monitor all socket events
- Uses SOCK-API to create non-blocking dual-stack (IPv4|6) sockets
- Uses RWBUG api to read and write lines to sockets
- Uses DB backend api to update key,value store
- DB backend support both an in memory store or mmap database file
- listens by default using wildcard [::]:6379
- cmd-line supports --help

**Example usage**

    $ ./server 
    [+] Database listening on [::]:6379

	$ ./server --help
	Usage: server [OPTIONS]

	Options:
	 --help          This help
	 --hostname      hostname to listen on
	 --port          port to listen on (default=6379)
	 --database      Path to database file
	 --log           log request/response
	 --argv          Dump argv to stdout

	Examples:
	  server --hostname 127.0.0.1 --port 6379 --database mydb.bin

### 1.3 Client

A telnet client that connect to a server address.  
Client simply reads and writes lines betwen stdio and server socket.

**Supported featues**

- cmd-line supports --help and selecting erver address and port
- Supports pipe or normal stdin,stdout

**Design**

- single-threaded application written in C with no 3rd party libs
- Uses a TCP wrapper or socket bridge between stdio/socket
- Uses a 4 way pipe to exchange lines between stdio/socket
- Reads lines from stdin and writes then to socket
- Reads lines from socket  and writes then to stdout
- Uses SOCK api to create socket and manage stdin,stdout fds
- Uses poll() to monitor fd activity (stdin,stdout,socket)
- Captures all error and logs them to stderr

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
	 --help          This help
	 --hostname      hostname to connect to
	 --port          port to listen on (default=6379)
	 --log           log request/response
	 --argv          Dump argv to stdout

	Examples:
	  client --hostname localhost --port 6379

## 2. Kubernetes

Deploy the the database client and server on Kubernetes.
    
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
    [+] Runing test-net
     => db-pod     -> internet        ✗ DENIED  [ PASS ]
     => client-pod -> internet        ✗ DENIED  [ PASS ]
     => client-pod -> db-pod:6379     ✓ ALLOWED [ PASS ]
     => random-pod -> db-pod:6379     ✗ DENIED  [ PASS ]
     => Ran 4 tests: 4 passed, 0 failed (100% success)
    ✅ test-k8s complete.


**Supported features**

 - make deploy   : fully automated k8s pod provisioning 
 - make test-k8s : fully automated test suite
 - make list-pod|list-net|show-log
 - All requested features where implemented (k8s folder)

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

## Testing the Project

Testing cmds, launcher and k8s requires a large test-suite.

The test targets are listed below.


- **make test**  run basic tests
- **make test-full** run all tests (test-cmds,test-lau,test-k8s)   
&nbsp;
- **make test-cmds** run cmd tests client,server
- **make test-lau** run ./launcher tests (using VM)
- **make test-k8s** run k8s tests (wait-pods, test-pod,test-net)   
&nbsp;
- **make test-server** run ./server tests
- **make test-client** run ./client tests
- **make wait-pods** wait for pods to come up
- **make test-pod** run pod GET|SET|DEL tests
- **make test-net** run network policy tests
