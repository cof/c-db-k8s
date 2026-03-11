# db-k8s
A simple client, server, laucjer

- **server**  A TCP server that implement SET|GET|DEL cmds to DB
- **client**  A TCP client suppors a telnet like connection to server
- **launcher**  A Linux container runtime management tool

## Prerequisites

- **GCC**: Version 9.0 or higher.
- **make**: Version 9.0 or higher.
- **Bash**: Version 4.0+ for the test runner.

## Building the Project

- **make all** (Default): Compiles server,client,launcher
- **make install** Copy all binaries to bin folder
- **make install-vm** Download and create a test-launcher VM
- **make list-vm**  Show test-launcher VM status
- **make list-cache** Show vm iso downloads
- **make wipe-vm** shutdown and undefine the VM
- **make gen-seccomp** generate a new seccomp rules flle
- **make rootfs** generate a roofs for the containers
- **make test** : Compiles all binaries and runs server,client tests
- **make clean**: Removes all compiled binaries, object files and test logs

## Design Notes

- All database logic in db.c and API exposed: in db.h 
- String processing adn  logging in util.c and API exposed in util.h
- server uses epoll and DB api to support simple SET|GET|DEL api
- client is very simple stdin/sdout/socket read/put line program
- launcher is a linux container runtime mangement tool
- All binaries are statically linked to prevent ldd issues
- An Alpine Linux VM was used to test launcher

## 1. Server
A TCP server than support a telnet-style api to acccsss a key/value store.  
Clients simply connect to server and send commands to modify key/value store.

**Supported Commands:**

- SET key value - store a key value
- GET key       - retrive a key value
- DEL key       - delete key/value from store
- QUIT          - close connection

**Supported featues**

- Implements a telnet style cmd SET|GET|DEL api to a DB

**Design**

- Uses signal to catch SIGTERM and SIGINT
- Uses getaddrinfo to select a socket address
- Uses socket/bind/listen/accept for network I/O
- Uses non blocking sockets
- Use dual stack sockets that support both IPv4 and IPv6
- Uses epoll (level trigged) to monitor all socket events
- Uses a socket wrapper api to read/write/track/log/errors
- Uses util string api to process strings
- Uses a readline wrapper around client sockets
- listens by default using wildcard [::]:6379
- cmdline can override this

**Example usage**

    $ ./server 
    [+] Database listening on [::]:6379

## 2. Client
A simple telnet client that connect to a server address.  
Client will connet to server and read/write line to server.

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


**Supported featues**

- Usea a simple C api for container configuration
- Create an manages its own network namespaces

**Design**

- Creates its own runtime dirs for network and filesystem namepaces
- By default it create runtime dir in the current dir
- Support cmd line args to allow runtime defaults
- Uses signal to catch SIGTERM and SIGINT
- Uses a container api to configure add an list of container to run.
- Create and manages its own netwok namespace files
- Supports running container in order or parallel
- Uses setns and  clone to run containers in their own netns
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

