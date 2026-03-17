#
# Makefile for c-db-k8s
#
# First run just do:
#
#  	make test
#
# Important targets
# 
#  all         : build cmds (client|server|launcher)
#  install     : put all cmds into bin folder
#  deploy      : builds images|create cluster|load images|deploy pods
# 
#  test        : build and test everything
#  test-cmds   : build and test cmds (client|server)
#  test-server : build and test ./server
#  test-client : build and test ./client
#  wait-pods   : wait for all pods to be ready
#  test-pod    : test SET|GET|DEL cmds on pods
#  test-net    : test k8s network policy
#
#  clean       : Remove compiled binaries, object files, and test logs
#  clean-k8s   : Remove cluster and docker images
#  spotless    : wipe everthing
#  
# ------------------------------------
#
# Deps
# ====
# - gcc build tools
# - awk for tests
# - docker - images
# - k3d - cluster
# 


# #######################
#     Config
# #######################
BUILD_DIR = build
SRC_DIR = src
BIN_DIR = bin
SCRIPTS_DIR = scripts
CMDS = server client launcher

# build tools
# -----------
INSTALL = install
TAR = tar
CC = gcc
LD = gcc
CTAGS = ctags
K3D = k3d


# verbosity - aka Kbuild/HAProxy style
# -----------------------------------
V ?= 0
Q = @
ifeq ($V,1)
Q=
endif

ifeq ($(V),1)
cmd_TAR = $(TAR)
cmd_CC  = $(CC)
cmd_LD  = $(CC)
else
cmd_TAR  = $(Q)echo "  TAR   $@";$(TAR)
cmd_CC   = $(Q)echo "  CC    $@";$(CC)
cmd_LD   = $(Q)echo "  LD    $@";$(CC)
endif

# compiler flags
# --------------
NO_EXTRA = -Wno-missing-field-initializers
CFLAGS += -D_GNU_SOURCE -Wall -Werror -Wextra $(NO_EXTRA) -O2 -Isrc -MMD -MP

DEBUG ?= 0
VALGRIND ?= 0

# debug build
ifeq ($(DEBUG), 1)
	CFLAGS += -O0 -ggdb3
endif

# valgrind reports errors when using static
ifeq ($(VALGRIND), 0)
	LDFLAGS = -static
endif


# Default target - build cmds
# --------------------------
.PHONY: all
all: $(CMDS) | $(BUILD_DIR)

$(BUILD_DIR):
	@mkdir -p $@

# where cmds are installed
$(BIN_DIR):
	@mkdir -p $@

# server
# ------
SERVER_SRCS = src/util.c src/log.c src/sock.c src/db.c src/server.c
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
SERVER_DEPS = $(SERVER_OBJS:.o=.d)
-include $(SERVER_DEPS)
server: $(SERVER_OBJS)
	$(cmd_LD) $(CFLAGS) $(LDFLAGS) $(SERVER_OBJS) -o $@

# client
# ------
CLIENT_SRCS = src/util.c src/log.c src/sock.c src/client.c
CLIENT_OBJS = $(CLIENT_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CLIENT_DEPS = $(CLIENT_OBJS:.o=.d)
-include $(CLIENT_DEPS)
client: $(CLIENT_OBJS)
	$(cmd_LD) $(CFLAGS) $(LDFLAGS) $(CLIENT_OBJS) -o $@

# launcher
# --------
LAUNCHER_LIBS = $(SECURITY_LIBS)
LAUNCHER_SRCS = src/util.c src/log.c src/ns_util.c src/launcher.c
LAUNCHER_OBJS = $(LAUNCHER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
LAUNCHER_DEPS = $(LAUNCHER_OBJS:.o=.d)
-include $(LAUNCHER_DEPS)
launcher: $(LAUNCHER_OBJS)
	$(cmd_LD) $(CFLAGS) $(LDFLAGS) $(LAUNCHER_OBJS) $(LAUNCHER_LIBS) -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(cmd_CC) $(CFLAGS) -c $< -o $@

# tags file
# ----------
.PHONY: tags
SOURCES = $(wildcard src/*.c src/*.h)
tags: $(SOURCES)
	@echo "Creating tags file"
	$(Q)$(CTAGS) $(SOURCES)

# generate seccomp
# -----------------
.PHONY: gen-seccomp
CMD_FILE   = tests/test_req.txt
GEN_SECCOMP = $(SCRIPTS_DIR)/gen_seccomp.awk
STRACE_SRV = $(BUILD_DIR)/strace_srv.txt
STRACE_CLI = $(BUILD_DIR)/strace_cli.txt
STRACE_RAW = $(BUILD_DIR)/strace_raw.txt
SECCOMP_H  = $(BUILD_DIR)/seccomp_rules.h
gen-seccomp: client server
	@echo "Generating seccomp rules..."
	@strace -c -f -o $(STRACE_SRV) ./server localhost:$(TEST_PORT) & STRACE_PID=$$!; \
	SERV_PID=$$(pgrep -P $$STRACE_PID); \
	echo "Profiling Server (PID: $$SERV_PID) via Strace (PID: $$STRACE_PID)"; \
	sleep 1; \
	strace -c -f -o $(STRACE_CLI) ./client localhost:$(TEST_PORT) < $(CMD_FILE) 1>/dev/null || true; \
	sleep 1; \
	pgrep -ax server || true; \
	kill $$SERV_PID  2>/dev/null || true; \
	kill $STRACE_PID 2>/dev/null || true; \
	pkill -x server || true
	pgrep -ax server || true
	@cat $(STRACE_SRV) $(STRACE_CLI) > $(STRACE_RAW);
	@awk -f $(GEN_SECCOMP) $(STRACE_RAW) > $(SECCOMP_H)
	@echo "SUCCESS: $(SECCOMP_H)"

# Build a roofs for OverlayFS
# ---------------------------
.PHONY: rootfs
OUR_CMDS = client server
AUX_CMDS = bash ls ip ping hostname
ROOTFS_DIR = rootfs
ROOTFS_DONE = $(BUILD_DIR)/.rootfs_done
rootfs: $(ROOTFS_DONE)

$(ROOTFS_DONE): $(OUR_CMDS) | $(BUILD_DIR)
	@echo "  BUILD $(ROOTFS_DIR)"
	@mkdir -p $(ROOTFS_DIR)  $(ROOTFS_DIR)/bin $(ROOTFS_DIR)/lib
	@echo "  + binaries - $(OUR_CMDS)"
	@for cmd in $^; do \
		install -D $$cmd $(ROOTFS_DIR)/bin; \
	done
	@echo "  + helpers - $(AUX_CMDS)"
	@for cmd in $(AUX_CMDS); do \
		CMD_PATH=$$(command -v $$cmd) || { echo "Error: $$cmd not found"; exit 1; }; \
	    install -D $$CMD_PATH $(ROOTFS_DIR)/bin/; \
	done 
	@echo "  + Shared libraires"
	@ldd $(ROOTFS_DIR)/bin/* | grep "=> /" | awk '{print $$3}' | sort -u | \
			xargs -I '{}' install -D '{}' $(ROOTFS_DIR)/lib/
	@echo "  + Dynamic Linker"
	@LOADER_PATH=$$(readelf -l $(ROOTFS_DIR)/bin/bash |
		grep "program interpreter" | awk '{print $$NF}' | tr -d '[]') ; \
		install -D $$LOADER_PATH $(ROOTFS_DIR)/lib/$${LOADER_PATH}
	@echo "  + created $(ROOTFS_DIR)"
	@touch $@

# package rootfs
$(ROOTFS_TAR) : $(ROOTFS_DONE)
	$(cmd_TAR) -czf $@ $(ROOTFS_DIR)

# install cmds into bin
# ---------------------
.PHONY: install
INSTALL_DONE=$(BUILD_DIR)/.install_done
BIN_SERVER=$(BIN_DIR)/db/server
BIN_CLIENT=$(BIN_DIR)/client/client
BIN_CMDS= $(BIN_CLIENT) $(BIN_SERVER)
install: $(INSTALL_DONE)
$(INSTALL_DONE): $(CMDS) | $(BUILD_DIR) $(BIN_DIR) 
	@echo "[Installing files]"
	$(INSTALL) -D -m 755 server $(BIN_DIR)/db/server
	$(INSTALL) -D -m 755 client $(BIN_DIR)/client/client
	$(INSTALL) -D -m 755 launcher $(BIN_DIR)
	touch $(INSTALL_DONE)


# ###########################
# Kubernetes Deployment
# ###########################

CLUSTER_NAME=db-k8s
SERVER_IMG=db-k8s-server:v1
CLIENT_IMG=db-k8s-client:v1

DOCKER_DONE=$(BUILD_DIR)/.docker_done
CLUSTER_DONE=$(BUILD_DIR)/.cluster_done
LOAD_DONE=$(BUILD_DIR)/.load_done
DEPLOY_DONE=$(BUILD_DIR)/.deploy_done
DONE_FILES = $(DOCKER_DONE) $(CLUSTER_DONE) $(LOAD_DONE) $(DEPLOY_DONE) $(WAIT_DONE)

# create docker images
# --------------------
.PHONY: build-images 
build-images: $(DOCKER_DONE)
$(DOCKER_DONE) : $(INSTALL_DONE) | $(BUILD_DIR)
	@echo "Building Docker images..."
	docker build --build-arg BIN_NAME=$(BIN_SERVER) -t $(SERVER_IMG) -f docker/server.Dockerfile .
	docker build --build-arg BIN_NAME=$(BIN_CLIENT) -t $(CLIENT_IMG) -f docker/client.Dockerfile .
	touch $(DOCKER_DONE)

# ensure cluster exists
# --------------------
.PHONY: create-cluster
create-cluster: $(CLUSTER_DONE)
$(CLUSTER_DONE): | $(BUILD_DIR)
	k3d cluster list | grep -qx $(CLUSTER_NAME) || k3d cluster create $(CLUSTER_NAME)
	touch $(CLUSTER_DONE)

# load docker images
# ------------------
.PHONY: load-images
load-images: $(LOAD_DONE)
$(LOAD_DONE): $(DOCKER_DONE) $(CLUSTER_DONE) | $(BUILD_DIR) 
	k3d image import $(SERVER_IMG) -c $(CLUSTER_NAME)
	k3d image import $(CLIENT_IMG) -c $(CLUSTER_NAME)
	touch $(LOAD_DONE)

# deploy the pods
# ---------------
.PHONY: deploy
deploy : $(DEPLOY_DONE)
$(DEPLOY_DONE): $(LOAD_DONE) | $(BUILD_DIR)
	@echo "Applying k8s manifest..."
	kubectl apply -k k8s/
	@echo "Restarting pods..."
	kubectl rollout restart deployment/client-app
	kubectl rollout restart statefulset/server-pod
	touch $(DEPLOY_DONE)

# misc k8s commands
# -----------------
.PHONY: apply
apply:
	kubectl apply -k k8s/

.PHONY: list-pod
list-pod:
	@printf '%66s\n' | tr ' ' '-'
	@kubectl get statefulset/server-pod deployment/client-app
	@printf '%66s\n' | tr ' ' '-'
	@kubectl get pods -l 'app in (client-pod,db-pod)'

.PHONY: list-all
list-all:
	kubectl get all

.PHONY: show-log
show-log:
	kubectl logs statefulset/server-pod | tail -n 10
	kubectl logs deployment/client-app  | tail -n 10

# kubectl logs -l app=client-pod -f --prefix
# kubectl logs -l app=server-db -f --prefix

# ###########################
# TEST SUITE MACROS & TARGETS
# ###########################

TEST_REQ_FILE = tests/test_req.txt
TEST_RSP_FILE = tests/test_rsp.txt
TEST_PORT = 6379
TEST_ADDR = 127.0.0.1
TEST_SERVER_LOG = $(BUILD_DIR)/test-server.log
TEST_WAIT_RUN = 0.5

WAIT_UP = timeout $(1) bash -c 'until nc -z $(2) $(3) 2>/dev/null; do sleep 0.1; done'
KILL_WAIT = kill $(1) 2>/dev/null;  wait $(1) 2>/dev/null || true

TEST_CMD = \
	total=$$((total + 1));  \
	echo "$(1)" | nc -w 1 -N $(TEST_ADDR) $(TEST_PORT) | \
	grep -q "$$EXPECT" && \
    echo " => TEST '$(1)' PASSED" || \
    (echo " => TEST '$(1)' FAILED"; errors=$$((errors + 1));  )

TEST_FILE = \
	total=$$((total + 1));  \
	diff -q $(1) $(2) && echo " => TEST $(1) PASSED" || \
	(echo " => TEST '$(1)' FAILED"; errors=$$((errors + 1)); )

TEST_REPORT = \
	passed=$$((total - errors)); \
	[ $$total -eq 0 ] && percent=100 || percent=$$(( (total - errs) * 100 / total )); \
	echo " => Ran $$total tests: $$passed passed, $$errors failed ($$percent% success)"

BUILD_REQ_FILE = $(BUILD_DIR)/$(notdir $(TEST_REQ_FILE))
BUILD_RSP_FILE = $(BUILD_DIR)/$(notdir $(TEST_RSP_FILE))
SIMPLE_SERVER = scripts/simple_server.awk

# pod macros
PASS_STR = "\033[32mPASS (Isolated)\033[0m\n"
FAIL_STR = "\033[31mFAIL (Leaking!)\033[0m\n"

REQ_START := $(subst ",,"[+] send req: ")
RSP_START := $(subst ",," recv rsp: ")
CLIENT_OK := $(subst ",,"[+] Connectivity test: OK")

TEST_POD_LOG = $(BUILD_DIR)/testpod.txt
TEST_POD_RES = $(TEST_POD_LOG).result
GET_APP    = kubectl get pods -l app=$(1) -o name | head -n 1
SND_ATTACH = echo $(1) | kubectl attach -qi $(2)
GET_LOGS = kubectl logs $(1) --tail=20 >$(2) 2>/dev/null
DO_CLEAN = sed -e 's/^> //' -e '/^\[+\]/!d' $(1) | \
	sed -z 's/\n\[+\] recv rsp:/ recv rsp:/g' > $(2)

DO_SEARCH  = grep -Fq "$(REQ_START)$(1)$(RSP_START)$(2)" $(3)
DO_REPORT  = && echo " => check $(1) [ PASS ]" || \
	{ echo " => check $(1) [ FAIL ] (Expected $(2))"; errors=$$((errors + 1)); }
DO_MATCH =  $(call DO_SEARCH,$(1),$(2),$(3)) $(call DO_REPORT,$(1),$(2))

.PHONY: test
test: test-cmds wait-pods test-pod test-net

# test cmds (client|server) TODO launcher - need VM)
# -------------------------------------------------
.PHONY: test-cmds
test-cmds: test-server test-client

# test ./server
# -----------------
.PHONY: test-server
test-server: server
	$(Q)echo "[+] Running $@"; \
	./server --hostname $(TEST_ADDR) --port $(TEST_PORT) 1> $(TEST_SERVER_LOG) 2>&1 & SRV_PID=$$!; \
	echo " => Starting server PID $$SRV_PID"; \
	sleep $(TEST_WAIT_RUN); \
	kill -0 $$SRV_PID || { echo " => server died - check log"; exit 1; }; \
	echo " => Waiting for $(TEST_ADDR):$(TEST_PORT)"; \
	$(call WAIT_UP,3,$(TEST_ADDR),$(TEST_PORT)) || \
		{ echo " => Wait failed";i $(call KILL_WAIT,$$SRV_PID); exit 1; }; \
	echo " => Server is UP Running tests..."; \
	total=0; errors=0; \
	$(call TEST_CMD,SET foo bar,OK); \
	$(call TEST_CMD,GET foo,bar); \
	$(call TEST_CMD,DEL foo,OK); \
	$(call TEST_CMD,GET foo,FAIL); \
	$(call TEST_CMD,SET key value1,OK); \
	$(call TEST_CMD,GET key,value1); \
	$(call TEST_CMD,SET key value2, OK); \
	$(call TEST_CMD,GET key,value2); \
	echo " => Shutting down server PID $$SRV_PID"; \
	$(call KILL_WAIT,$$SRV_PID); \
	$(call TEST_REPORT); \
	if [ $$errors -gt 0 ]; then exit 1; fi

# test ./client 
# ----------------
.PHONY: test-client
test-client: client
	$(Q)echo "[+] Running $@"; \
	rm -f $(BUILD_REQ_FILE) $(BUILD_RSP_FILE); \
	awk -f ./$(SIMPLE_SERVER) -v Port="$(TEST_PORT)" \
		 -v LogFile="$(BUILD_REQ_FILE)" -v RespFile="$(TEST_RSP_FILE)" \
		 1>$(TEST_SERVER_LOG) 2>&1 & SRV_PID=$$!; \
	echo " => Starting simple-server PID $$SRV_PID"; \
	sleep $(TEST_WAIT_RUN); \
	kill -0 $$SRV_PID || { echo " => simple-server died - check log"; exit 1; }; \
	echo " => Waiting for $(TEST_ADDR):$(TEST_PORT)"; \
	$(call WAIT_UP,3,$(TEST_ADDR),$(TEST_PORT)) || \
		{ echo " => Wait failed";i $(call KILL_WAIT,$$SRV_PID); exit 1; }; \
	echo " => Server is UP send $(TEST_REQ_FILE) via client"; \
	cat $(TEST_REQ_FILE) | timeout 2s ./client --hostname $(TEST_ADDR) --port $(TEST_PORT) > $(BUILD_RSP_FILE); \
	sed -i -e 's/^> //' -e '/^\[+]/d' $(BUILD_RSP_FILE); \
	echo " => Shutting down simple-server PID $$SRV_PID"; \
	$(call KILL_WAIT,$$SRV_PID); \
	total=0; errors=0; \
	$(call TEST_FILE,$(TEST_REQ_FILE),$(BUILD_REQ_FILE)); \
	$(call TEST_FILE,$(TEST_RSP_FILE),$(BUILD_RSP_FILE)); \
	$(call TEST_REPORT); \
	if [ $$errors -gt 0 ]; then exit 1; fi

# wait for all pods to be ready
# ----------------------------
.PHONY:wait-pods
wait-pods: deploy
	$(Q)echo "[+] Waiting for cluster to be READY..."
	$(Q)echo " => Waiting for db-pods ..."
	$(Q)kubectl wait --for=condition=Ready pod -l app=db-pod --timeout=30s | sed 's/^/ => /'
	$(Q)echo " => Waiting for client pods ..."
	$(Q)kubectl wait --for=condition=Ready pod -l app=client-pod --timeout=30s | sed 's/^/ => /'
	$(Q)echo "[+] Waiting for $(CLIENT_OK)"; \
	count=0; \
	until kubectl logs -l app=client-pod --tail=10 2>/dev/null | grep -Fq "$(CLIENT_OK)"; do \
		if [ $$count -eq 3 ]; then \
			echo " => [ERROR} timeout waitiig for $(CLIENT_OK) in logs"; \
			exit 1; \
		fi; \
		printf "."; \
		sleep 1; \
		count=$$((count + 1)); \
	done; echo "[+} Pods ready"


# test SET|GET|DEL via client-pods
# --------------------------------
.PHONY: test-pod
test-pod: deploy
	$(Q)echo "[+] Running $@"; \
	CLIENT_POD=$$($(call GET_APP,client-pod)); \
	echo " => Using client-pod: $$CLIENT_POD"; \
	RAND_STR=$$(LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 30); \
	CMD1="SET test-pod $$RAND_STR"; \
	CMD2="GET test-pod"; \
	CMD3="DEL test-pod"; \
	echo " => Sending cmds"; \
	$(call SND_ATTACH,$$CMD1,$$CLIENT_POD); \
	$(call SND_ATTACH,$$CMD2,$$CLIENT_POD); \
	$(call SND_ATTACH,$$CMD3,$$CLIENT_POD); \
	echo " => Fetching logs"; \
	$(call GET_LOGS,$$CLIENT_POD,$(TEST_POD_LOG)); \
	$(call DO_CLEAN,$(TEST_POD_LOG),$(TEST_POD_RES)); \
	echo " => Checking results"; \
	errors=0; \
	$(call DO_MATCH,$$CMD1,OK,$(TEST_POD_RES)); \
	$(call DO_MATCH,$$CMD2,$$RAND_STR,$(TEST_POD_RES)); \
	$(call DO_MATCH,$$CMD3,OK,$(TEST_POD_RES)); \
	if [ $$errors -gt 0 ]; then echo "!!! Total Failures: $$errors"; exit 1; fi; \

# test k8s Network Policy
# -----------------------
.PHONY: test-net
test-net: deploy
	$(Q)echo "[+] Runing $@"; \
	MYPOD=$$($(call GET_APP,db-pod)); \
	echo -n " => Checking db-pod -> internet "; \
	kubectl exec $$MYPOD -- nc -w 3 -zv google.com 443 >/dev/null 2>&1; \
	if [ $$? -ne 0 ]; then printf $(PASS_STR); else printf $(FAIL_STR); exit 1; fi; \
	echo -n " => Checking client-pod -> internet "; \
	MYPOD=$$($(call GET_APP,client-pod)); \
	kubectl exec $$MYPOD -- nc -w 3 -zv google.com 443 >/dev/null 2>&1; \
	if [ $$? -ne 0 ]; then printf $(PASS_STR); else printf $(FAIL_STR); exit 1; fi; \
	echo -n " => Checking client-pod -> db-pod "; \
	MYPOD=$$($(call GET_APP,client-pod)); \
	kubectl exec $$MYPOD -- nc -w 3 -zv db-service $(TEST_PORT) >/dev/null 2>&1; \
	if [ $$? -eq 0 ]; then printf $(PASS_STR); else printf $(FAIL_STR); exit 1; fi; \
	echo -n " => Checking random-pod -> db-pod:$(TEST_PORT) "; \
	kubectl run random-pod --image=busybox -l app=random --restart=Never -- sleep 30 >/dev/null 2>&1; \
	kubectl wait --for=condition=Ready pod/random-pod --timeout=15s >/dev/null 2>&1; \
	kubectl exec random-pod -- nc -w 3 -zv db-service $(TEST_PORT) >/dev/null 2>&1; \
	EXIT_CODE=$$?; \
	kubectl delete pod random-pod --now >/dev/null 2>&1; \
	if [ $$EXIT_CODE -ne 0 ]; then printf $(PASS_STR); else printf $(FAIL_STR); exit 1; fi

# #######################
# VM for testing launcher
# #######################

# alpine linux image
# ------------------
OS_VARIANT= alpinelinux3.21
OS_NAME=alpine
REL_VER= 3.21
PATCH_VER=.6
REL_NAME = $(OS_NAME)-$(REL_VER)$(PATCH_VER)
REL_FILE = nocloud_$(REL_NAME)-x86_64-bios-cloudinit-r0.qcow2
REL_DIR = v$(REL_VER)/releases/cloud
MIRROR = https://dl-cdn.alpinelinux.org
REL_URL = $(MIRROR_URL)/$(REL_DIR)/$(REL_FILE)

# our vm
VM_NAME = test-launcher
VM_FILE = myalpine.qcow2
VMDIR = vmdir

ifeq ($(origin CACHE_DIR), undefined)
  CACHE_DIR := $(shell echo $${XDG_CACHE_HOME:-$$HOME/.cache}/my-vm-project)
endif

BASE_IMAGE = $(CACHE_DIR)/$(REL_FILE)
RUN_IMAGE = $(VMDIR)/$(VM_FILE)
VM_MAC := 52:54:00:12:34:56
VM_IP  := 192.168.122.243

# alpine VMs  use user-data.yaml to autoconfigure
USER_DATA = tests/user-data.yaml

.PHONY: show-config
show-config:
	@echo "MIRROR=$(MIRROR)"
	@echo "REL_URL=$(REL_URL)"
	@echo "REL_FILE=$(REL_FILE)"
	@echo "REL_VER=$(REL_VER)"
	@echo "CACHE_DIR=$(CACHE_DIR)"
	@echo "BASE_IMAGE=$(BASE_IMAGE)"
	@echo "RUN_IMAGE=$(RUN_IMAGE)"

$(CACHE_DIR):
	mkdir -p $@

$(VMDIR):
	mkdir -p $@

# download image 
$(BASE_IMAGE) : | $(CACHE_DIR)
	wget -nv --no-verbose --show-progress -O $@.tmp $(REL_URL)
	mv $@.tmp $@
	chmod 444 $@

# build image-  XXX  virt-customize requires root read /boot/vmlinux ???
#$(RUN_IMAGE) : $(BASE_IMAGE)
#	@echo "Setting up image file"
#	@cp $(IMAGE_PATH) $(TEST_IMAGE)
#	virt-customize -a $(TEST_IMAGE) \
#	--root-password password:test \
#	--install openssh \
#	--edit '/etc/ssh/sshd_config: s/\#PermitRootLogin.*/PermitRootLogin yes/' \
#	--run-command "rc-update add sshd"
#	@touch $(TEST_IMAGE)

$(RUN_IMAGE): | $(BASE_IMAGE) $(VMDIR)
	cp $(BASE_IMAGE) $@

.PHONY:list-cache
list-cache: $(CACHE_DIR)
	ls -lh $(CACHE_DIR)

.PHONY:install-vm
install-vm: $(RUN_IMAGE)
	virt-install \
	--name $(VM_NAME) \
	--virt-type kvm \
	--ram 512 \
	--vcpus 1 \
	--disk path=$(RUN_IMAGE),format=qcow2,bus=virtio \
	--network network=default,model=virtio \
	--cloud-init user-data=$(USER_DATA) \
	--os-variant $(OS_VARIANT) \
	--graphics vnc \
	--rng /dev/urandom \
	--noautoconsole \
	--import

.PHONY: start-vm
start-vm:
	virsh start $(VM_NAME)

.PHONY: list-vm
list-vm:
	virsh dominfo $(VM_NAME) || true
	virsh domifaddr $(VM_NAME) || true

wipe-vm:
	 virsh destroy $(VM_NAME)  || true
	 virsh undefine $(VM_NAME) || true
	 rm -fr vmdir

# #######################
# 		Cleanup
# #######################

# delete cluster and docker images
# -------------------------------
clean-k8s:
	@echo "Cleaning k8s config"
	k3d cluster delete $(CLUSTER_NAME) || true
	docker rmi $(SERVER_IMG) $(CLIENT_IMG) || true
	rm -f $(DONE_FILES)

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(ROOTFS_DIR) $(CMDS) $(BIN_DIR) tags

.PHONY: clean-all
clean-all: clean-k8s clean
	@echo "Clean done"

.PHONY: spotless
spotless: clean-all
	@echo "Wiping everthing"
	docker system prune -af --volumes
