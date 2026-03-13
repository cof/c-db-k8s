# db-k8s

Run a database client server applications inside containers.

- **Local Containers**  run DB client/server inside a custom container
- **Kubernetes**   Deploy DB client/server to real k8s setup

## Prerequisites

- **GCC**: Version 9.0 or higher.
- **make**: Version 4.0 or higher.
- **Bash**: Version 4.0+ for running tests
- **awk**: Version 4.0+ for running tests
- **Docker**: for building container images
- **kind**:  for creating clusters and loading docker images
- **k8s**:  for managing pods

## Building the Project

- **make all** (Default): Compiles server,client,launcher
- **make deploy**  Deploy client and server into k8s
- **make test** : Runs server,client tests
- **make clean**: Removes compiled binaries, object files, k8s artifacts and test logs
- **make spotless**: clean + docker system prune

Misc targets

- **make install** Copy all binaries to bin folder
- **make install-vm** Download and create a test-launcher VM
- **make list-vm**  Show test-launcher VM status
- **make list-cache** Show VM iso downloads
- **make show-config** Show VM config
- **make wipe-vm** shutdown VM, undefine VM and remove the VM file
- **make gen-seccomp** generate a new seccomp rules flle
- **make rootfs** generate a roofs for the containers


## 1. Local Container Deployment

Simply runs a database client and server inside a custom containers.

- **server**  A TCP server that implement SET|GET|DEL cmds to DB
- **client**  A TCP client suppors a telnet like connection to server
- **launcher**  A Linux container runtime management tool

## Design Notes

- All database logic in db.c and API exposed: in db.h 
- String processing adn  logging in util.c and API exposed in util.h
- server uses epoll and DB api to support simple SET|GET|DEL api
- client is very simple stdin/sdout/socket read/put line program
- launcher is a linux container runtime mangement tool
- All binaries are statically linked to prevent ldd issues
- An Alpine Linux VM was used to test launcher

### 1.1 Server
A TCP server than support a telnet-style api to acccsss a key/value store.  
Clients simply connect to server and send commands to modify key/value store.

**Supported Commands:**

- SET key value - store a key value
- GET key       - retrive a key value
- DEL key       - delete key/value from store
- QUIT          - close connection

**Supported featues**

- Implements a telnet style cmd SET|GET|DEL api to a DB
- Implements a DB using mmap database files

**Design**

- Uses signal to catch SIGTERM and SIGINT
- Uses mmap to create a database file
- Uses getaddrinfo to select a socket address
- Uses socket/bind/listen/accept for network I/O
- Uses non blocking sockets
- Use dual stack sockets that support both IPv4 and IPv6
- Uses epoll (level triggered) to monitor all socket events
- Uses a socket wrapper api to read/write/track/log/errors
- Uses util string api to process strings
- Uses a custom read-line wrapper around client sockets
- listens by default using wildcard [::]:6379
- cmd-line can override this

**Example usage**

    $ ./server 
    [+] Database listening on [::]:6379

### 1.2 Client
A simple telnet client that connect to a server address.  
Client will connect to server and read/write lines to server.

**Supported featues**

- Connect to a server addres and port
- read and writes line to a socket

**Design**

- Uses getaddrinfo() to get a server TCP address
- Uses socket/connect/fdopen to read/write network I/O
- Uses fdopen to wrap socket into  read/write FIlE.
- Uses fgets/fputs to read/write lines to server
- Captures all error and logs them to stderr


**Example usage**

	$ ./client 127.0.0.1
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


## Launcher
A linux runtime container launcher for running server and client in isolated namespaces:


**Supported features**

- Create an manages its own network namespaces
- Supports privilege dropping (sudo,caps,privs,seccomp)
- Use a simple API for container configuration

**Design**

- Creates its own runtime dirs for network and filesystem namepaces
- By default it create runtime dir in the current dir
- Support cmd line args to overide runtime defaults
- Uses signal to catch SIGTERM and SIGINT
- Uses a container api to configure a list of containers to run.
- Create and manages its own network namespace files
- Supports running container in order or in parallel
- Uses bind mount to create network namespace files
- Uses setns and clone to run containers in their own netns
- Uses pipe to sync with child 
- Child setup proc and its own rootfs before excing containerA
- Child supports privilege dropping sudo|caps|priv|seccomp
- Parent uses waitpid to monitor childrean
- If any container dies it kills/reaps the rest
- seccomp rules where built with strace and awk script
- An Alpine VM image was used to test launcher
- You can uses make install-vm to recreate vm
- Uss cloud-config yaml to configure VM

**Example usage**

	$ virsh start test-launcher
	Domain 'test-launcher' started
	$ make install                                     
	install -D -m 755 server bin/db/server                                       
	install -D -m 755 client bin/client/client                                   
	install -D -m 755 launcher bin                                               
	$ scp -r bin alpine@test-launcher:
	client    100% 1153KB  52.7MB/s   00:00
	launcher  100% 1279KB 139.0MB/s   00:00    
	server    100% 1179KB 219.0MB/s   00:00 

	$ ssh alpine@test-launcher
	Welcome to Alpine!
	$ launcher:~$ cd bin/
	launcher:~/bin$ sudo ./launcher 
	[+] Created network namespace: db-ns
	[+] Created network namespace: client-ns
	[+] Created veth pair: veth-client <-> veth-db
	[+] Database listening on [::]:6379
	[+] Connectivity test: OK
	> [+] Client connected from [::ffff:10.0.0.2]:52362
	SET foo bar
	OK
	> get foo
	bar
	> get hello there
	FAIL
	> del foo
	OK
	> quit
	[+] Client local-close [::ffff:10.0.0.2]:52362
	OK
	> 
	[+] Connection closed by foreign host.
	[+] Container 'client' exit ok (pid=2408 why=exit_code 0)
	[+] Server PID:1 shutting down: got signal 15 (Terminated) from UID:0 PID:0 
	launcher:~/bin$ 

## 2. Kubernetes

Deploy the the database client and server on Kubernetes.

To deploy simply run

    $ make deploy

This will:

- Compile the client,server,launcher
- Install them into bin folder
- build docker images files
- create a cluster
- load docker images into cluster
- start the k8s pods

To see the k8s pods simply run

    $ make list-pod

**Example usage**

    $ make list-pod
    kubectl get all
    NAME                              READY   STATUS    RESTARTS   AGE
    pod/client-app-86b75f877c-2xv4f   1/1     Running   0          76m
    pod/client-app-86b75f877c-dbfxg   1/1     Running   0          76m
    pod/client-app-86b75f877c-dj8rb   1/1     Running   0          76m
    pod/server-pod-0                  1/1     Running   0          76m

    NAME                 TYPE        CLUSTER-IP   EXTERNAL-IP   PORT(S)    AGE
    service/db-service   ClusterIP   None         <none>        6379/TCP   140m
    service/kubernetes   ClusterIP   10.96.0.1    <none>        443/TCP    140m

    NAME                         READY   UP-TO-DATE   AVAILABLE   AGE
    deployment.apps/client-app   3/3     3            3           140m

    NAME                                    DESIRED   CURRENT   READY   AGE
    replicaset.apps/client-app-59ccc6f6dc   0         0         0       131m
    replicaset.apps/client-app-67d945f567   0         0         0       131m
    replicaset.apps/client-app-67d9cf8b4d   0         0         0       134m
    replicaset.apps/client-app-76759d5bdf   0         0         0       133m
    replicaset.apps/client-app-798bcff9b7   0         0         0       135m
    replicaset.apps/client-app-7d54d6b68f   0         0         0       134m
    replicaset.apps/client-app-844b59d7b4   0         0         0       125m
    replicaset.apps/client-app-86b75f877c   3         3         3       76m
    replicaset.apps/client-app-89bdbd7b     0         0         0       77m
    replicaset.apps/client-app-9dc9d7fcf    0         0         0       126m
    replicaset.apps/client-app-f6f56c588    0         0         0       106m

    NAME                          READY   AGE
    statefulset.apps/server-pod   1/1     140m

