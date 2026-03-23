# db-k8s

Run a database client and server application inside containers.

- **Local Containers**  run DB client/server inside a custom container
- **Kubernetes**   Deploy DB client/server to real k8s setup

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
- **make install** Copy all binaries to bin folder
- **make deploy**  Deploy client and server into k8s pods
- **make clean**: Removes compiled binaries, object files, k8s artifacts and test logs
- **make spotless**: clean + docker system prune

Test targets

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

Misc targets:

- **make gen-seccomp** generate a new seccomp rules flle
- **make rootfs** generate a roofs for the containers

VM targets:

- **make vm-config** Show VM config
- **make vm-list**  Show test-launcher VM status
- **make vm-clean** shutdown, undefine, remove VM 

## 1. Local Containers

Runs a database client and server inside custom containers.

- **launcher** A Linux container runtime management tool
- **server**  A TCP server that implement SET|GET|DEL cmds to DB
- **client**  A TCP client supports a telnet like connection to server

## 1.1 Launcher

A linux runtime container launcher for running applications in isolated namespaces.

**Supported features**

- Create and manages its own network namespaces
- Creates an isolated filesystem (rootfs)
- Supports privilege dropping (sudo,caps,privs,seccomp)
- Use a simple API for container configuration
- An Alpine Linux VM was used to test launcher

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

### 1.2 Server
A TCP server than support a telnet-style api to acccsss a key/value store.  
Clients simply connect to server and send commands to modify key/value store.

**Supported Commands:**

- SET key value - store a key value
- GET key       - retrive a key value
- DEL key       - delete key/value from store
- QUIT          - close connection

**Supported featues**

- Supoorts a telnet style cmd SET|GET|DEL api to a DB
- Database using mmap database file

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

### 1.3 Client
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



## 2. Kubernetes

Deploy the the database client and server on Kubernetes.
	
To deploy simply make deploy or make test-all

    $ make deploy

This will:

- Compile the client,server,launcher
- Install them into bin folder
- build docker images files
- create a cluster
- load docker images into cluster
- start the pods

**Supported featues**

####2.1 Database
- Database Deployment
- StatefulSet with single replica
- PersistentVolumeClaim for data
- Service for stable DNS name
- Resource limits and security context

####2.2 Client
- Deployment with configurable replicas
- Environment-based database connection
- Readiness probe that checks DB connectivity

####2.3 Network Policy
- Only client pods can reach database
- Database cannot initiate outbound connections
- Deny all other ingress to database

####2.4 Observability
- Client logs each request/response
- Database logs connections with source IP


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

To check the client and database work.

	$ make test-pod
	[INFO] Staring test-pod
	- Sending SET|GET cmds to client pod/client-app-644b4d99d-hfh8w
	- Checking client logs for N6gOogByp48KOp8OwgJvpOyOosBY7U
	- TEST passed

To check the Nework Policy is active.

	$ make test-net
	Checking db-pod -> internet blocked PASS (Isolated)
	Checking client-pod -> internet blocked: PASS (Isolated)
	Checking client-pod -> db-pod allowd: PASS (Isolated)
	Checking random-pod to db-pod:6379: PASS (Isolated)

