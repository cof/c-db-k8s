# db-k8s

Run a database (DB) client and server application inside containers.

There are 2 parts to this project. 

- **Containers**  run DB client and server inside containers using a custom launcher
- **Kubernetes**  Deploy DB client/server inside k8s pods

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
- **make test-full** Run all tests (cmds,laucher,k8s)
- **make spotless**: wipe complied binaries, VMS, k8s pods

## Design Notes

- Makefile has extensive build and test targets:
- All applicaton code written in C using custom apis
- SOCK api - socket layer wrapper in sock(.h|.c)
- RWBUF api - to read|write buffer data
- UTIL api - string process,cmd-line parsing, signal handling
- LOG api - info and erro loggin in log(.h|.c)

## Testing the Project
Note there are a lot test componets in project.

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

## 1. Containers

Runs a database (DB) client and server inside containers using custom launcher.

- **launcher** A Linux container runtime management tool
- **server**  A DB server that implement SET|GET|DEL cmds to DB
- **client**  A DB client supports a telnet like connection to server

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

- use UTIL api to read cmd-line args
- create dirs/inftrastructure before running containers
- Create a folder (rootfs) for each container to hold its rootfs
- create veth devices for container
- creates a network namespace for each container
- creates a child process for each container
- child switches to its private rootfs
- child creates proc
- child applys security settings
- child execs the client or server binary
- uses mount and clone to create netns and containers

**Testing **

- An Alpine VM image was used to safely test launcher
- Makefile has support for downloading and building VM
- Simply run make test-lau to download and create the VM
- Uses cloud-config user-data.yaml template file to configure VM
- Embeds users public key (id_pub.rsa) into user-data.yaml file
- VM alpine user is setup as sudo user (using doas)
- make test-lau simply installs client,server,launcher into VM and runs launcher
- Can simply ssh to VM with ssh alpine@test-lau if nsswitch.conf allows it
- Just edit /etc/nsswitch.conf and add libvirt_guest to hosts line e.g "hosts: files libvirt_guest" 

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
A database (DB) server than support a telnet-style api to acccsss a key/value store.  
Clients simply connect to server and send commands to modify key/value store.

**Supported Commands:**

- SET key value - store a key value
- GET key       - retrive a key value
- DEL key       - delete key/value from store
- QUIT          - close connection

**Supported featues**

- Supports a telnet style cmd SET|GET|DEL api to a DB
- Database using mmap database file

**Design**

- code is single threaded
- Use dual stack sockets that support both IPv4 and IPv6
- Uses SOCK-API to create non blocking listener and client sockets
- Uses epoll (level triggered) to monitor all socket events
- Uses RWBUG api to read and write lines to sockets
- Uses DB api to update key,value store
- listens by default using wildcard [::]:6379
- cmd-line can override this

**Example usage**

    $ ./server 
    [+] Database listening on [::]:6379

### 1.3 Client
A telnet client that connect to a server address.  
Client simply reads and writes lines betwen stdio and server socket.

**Supported featues**

- Connect to a server addres and port
- Client acts a 4 way pipe between stdio/socket
- Suppors pipe or normal stdin,stdout

**Design**

- Uses a TCP wrapper or socket bridge between stdio/socket
- Client acts a 4 way pipe between stdio/socket
- Reads lines from stdin and writes then to socket
- Reads lines from socket  and writes then to stdout
- Uses a 4 way pipe design to read and write lines:
- Uses SOCK api to create socket and manage stdin,stdout fds
- Uses poll() to monitor fd activity (stdin,stdout,socket)
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

