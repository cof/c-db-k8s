#
# Makefile for c-db-k8s
#
# First run just do:
#
#  make test-full
#
# Important targets
# 
#  all         : build cmds (client|server|launcher)
#  install     : put all cmds into bin folder
#  deploy      : builds images|create cluster|load images|deploy pods
#
#  debug       : debug build
#  asan        : asan debug build
#  valgrind    : valgrind debug build
# 
#  test        : run basic tests (test-cmds)
#  test-full   : run all tests (test-cmds,test-lau,test-k8s)
#
#  test-cmds   : run cmd tests (client,server)
#  test-lau    : run launcher tests (using VM)
#  test-k8s    : run k8s tests (wait-pods,test-pod,test-net)
#
#  test-server : build and test ./server
#  test-client : build and test ./client
#
#  wait-pods   : wait for all pods to be ready
#  test-pod    : test SET|GET|DEL cmds on pods
#  test-net    : test k8s network policy
#
#  clean       : Remove compiled binaries, object files, and test logs
#  clean-k8s   : Remove cluster and docker images
#  spotless    : wipe everthing
#
#  vm-config   : show vm config
#  vm-clean    : wipe vm
#  vm-list     : show vm
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
GCC_DEPS      := -MMD -MP
CPP_FLAGS     := -D_GNU_SOURCE -Isrc
EXTRA_CFLAGS  := -Wextra -Wno-missing-field-initializers
COMMON_CFLAGS := -Wall  -Werror=implicit-function-declaration $(CPP_FLAGS) $(GCC_DEPS)
DEBUG_CFLAGS  := -ggdb3 -fno-omit-frame-pointer -DDEBUG=1

# release build
# -----------
CFLAGS  = -O2 $(COMMON_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS = --static

MAKEFLAGS += --no-print-directory

# ###########################
# CMDS server|client|launcher
# ###########################

# Default target - build cmds
# --------------------------
.PHONY: all
all: $(CMDS) | $(BUILD_DIR)

# debug build
# -----------
debug: CFLAGS = -O0 $(COMMON_CFLAGS) $(DEBUG_CFLAGS)
debug: all

# asan build
# -----------
asan: CFLAGS = -O0 $(COMMON_CFLAGS) $(DEBUG_CFLAGS) -fsanitize=address 
asan: LDFLAGS = -fsanitize=address
asan: all

# valgrind build
# --------------
valgrind: CFLAGS = -O0 $(COMMON_CFLAGS) $(DEBUG_CFLAGS)
valgrind: LDFLAGS = 
valgrind: all

$(BUILD_DIR):
	@mkdir -p $@

# server
# ------
SERVER_SRCS = src/util.c src/log.c src/rwbuf.c src/sock.c src/db.c src/server.c
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
SERVER_DEPS = $(SERVER_OBJS:.o=.d)
-include $(SERVER_DEPS)
server: $(SERVER_OBJS)
	$(cmd_LD) $(CFLAGS) $(LDFLAGS) $(SERVER_OBJS) -o $@

# client
# ------
CLIENT_SRCS = src/util.c src/log.c src/rwbuf.c src/sock.c src/client.c
CLIENT_OBJS = $(CLIENT_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CLIENT_DEPS = $(CLIENT_OBJS:.o=.d)
-include $(CLIENT_DEPS)
client: $(CLIENT_OBJS)
	$(cmd_LD) $(CFLAGS) $(LDFLAGS) $(CLIENT_OBJS) -o $@

# launcher
# --------
LAUNCHER_SRCS = src/util.c src/log.c src/ns_util.c src/lau_child.c src/launcher.c
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

# seccomp filters
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
	@strace -c -f -o $(STRACE_SRV) ./server $(CMD_ARGS) & STRACE_PID=$$!; \
	SERV_PID=$$(pgrep -P $$STRACE_PID); \
	echo "Profiling Server (PID: $$SERV_PID) via Strace (PID: $$STRACE_PID)"; \
	sleep 1; \
	strace -c -f -o $(STRACE_CLI) ./client $(CMD_ARGS) < $(CMD_FILE) 1>/dev/null || true; \
	sleep 1; \
	pgrep -ax server || true; \
	kill $$SERV_PID  2>/dev/null || true; \
	kill $STRACE_PID 2>/dev/null || true; \
	pkill -x server || true
	pgrep -ax server || true
	@cat $(STRACE_SRV) $(STRACE_CLI) > $(STRACE_RAW);
	@awk -f $(GEN_SECCOMP) $(STRACE_RAW) > $(SECCOMP_H)
	@echo "SUCCESS: $(SECCOMP_H)"

# roofs for OverlayFS
# --------------------
ROOTFS_DIR  = $(BUILD_DIR)/rootfs
ROOTFS_DONE = $(BUILD_DIR)/.rootfs_done
OUR_CMDS =
AUX_CMDS = bash ls ip ping hostname
.PHONY: rootfs
rootfs: $(ROOTFS_DONE)
$(ROOTFS_DONE): $(OUR_CMDS) | $(BUILD_DIR)
	@echo "[+] Building rootfs"
	@mkdir -p $(ROOTFS_DIR) $(ROOTFS_DIR)/bin $(ROOTFS_DIR)/lib
	@echo " => Add binaries - $(OUR_CMDS)"
	$(Q)for cmd in $^; do \
		install -D $$cmd $(ROOTFS_DIR)/bin; \
	done
	@echo " => Add helpers - $(AUX_CMDS)"
	$(Q)for cmd in $(AUX_CMDS); do \
		CMD_PATH=$$(command -v $$cmd) || { echo "Error: $$cmd not found"; exit 1; }; \
	    install -D $$CMD_PATH $(ROOTFS_DIR)/bin/; \
	done 
	@echo " => Add Shared libraires"
	$(Q)ldd $(ROOTFS_DIR)/bin/* 2>/dev/null | grep "=> /" | awk '{print $$3}' | sort -u | \
			xargs -I '{}' install -D '{}' $(ROOTFS_DIR)/lib/
	@echo " => Add Dynamic Linker"
	$(Q)LOADER_PATH=$$(readelf -l $(ROOTFS_DIR)/bin/bash | \
		grep "program interpreter" | awk '{print $$NF}' | tr -d '[]') ; \
		install -D $$LOADER_PATH $(ROOTFS_DIR)/$${LOADER_PATH}
	@echo " => Created rootfs $(ROOTFS_DIR)"
	@rm -f $(INSTALL_DONE)
	@touch $@

# install cmds (and rootfs) into bin
# ----------------------------------
.PHONY: install
INSTALL_DONE=$(BUILD_DIR)/.install_done
BIN_SERVER=$(BIN_DIR)/db/server
BIN_CLIENT=$(BIN_DIR)/client/client
BIN_CMDS= $(BIN_CLIENT) $(BIN_SERVER)
install: $(INSTALL_DONE)
$(INSTALL_DONE): $(CMDS) | $(BUILD_DIR)
	@echo "[Installing files]"
	@mkdir -p $(BIN_DIR)
	$(INSTALL) -D -m 755 server $(BIN_DIR)/db/server
	$(INSTALL) -D -m 755 client $(BIN_DIR)/client/client
	$(INSTALL) -D -m 755 launcher $(BIN_DIR)
	$(Q)if [ -d $(ROOTFS_DIR) ]; then  \
		echo "Install $(ROOTFS_DIR) to $(BIN_DIR)"; \
		cp -a $(ROOTFS_DIR) $(BIN_DIR)/; \
	fi
	@touch $(INSTALL_DONE)

# #######################
# VM for testing launcher
# #######################

# alpine linux image file
# -----------------------
OS_VARIANT= alpinelinux3.21
OS_NAME=alpine
REL_VER= 3.21
PATCH_VER=.6
REL_NAME = $(OS_NAME)-$(REL_VER)$(PATCH_VER)
REL_FILE = nocloud_$(REL_NAME)-x86_64-bios-cloudinit-r0.qcow2
REL_DIR = v$(REL_VER)/releases/cloud
MIRROR = https://dl-cdn.alpinelinux.org
REL_URL = $(MIRROR)/$(REL_DIR)/$(REL_FILE)

# our vm
# ------
VM_NAME := test-lau
VM_FILE := myalpine.qcow2
VM_DIR  := vmdir
VM_USER := alpine
VM_HOME := /home/$(VM_USER)
VM_BIN_DIR := /home/$(VM_USER)/bin

# where we store downloads
# ------------------------
ifeq ($(origin VM_CACHE_DIR), undefined)
  VM_CACHE_DIR := $(shell echo $${XDG_CACHE_HOME:-$$HOME/.cache}/my-vm-project)
endif

VM_CACHE_FILE = $(VM_CACHE_DIR)/$(REL_FILE)
VM_DISK = $(VM_DIR)/$(VM_FILE)
#VM_MAC := 52:54:00:12:34:56
#VM_IP  := 192.168.122.243

# autoconfigure using user-data.yaml
# ---------------------------------
VM_USER_DATA = $(BUILD_DIR)/user-data.yaml
VM_DONE      = $(BUILD_DIR)/.vm_done

# get public key
# --------------
VM_SSH_KEYFILE := ~/.ssh/id_rsa
VM_PUB_KEYFILE := $(VM_SSH_KEYFILE).pub
VM_SSH_PUBKEY  := $(shell cat $(VM_PUB_KEYFILE) 2>/dev/null || echo "NO_KEY_FOUND")

# create cloud-init user-data
# ---------------------------
$(VM_USER_DATA): tests/user-data.yaml | $(BUILD_DIR)
	$(Q)if [ "$(VM_SSH_PUBKEY)" = "NO_KEY_FOUND" ]; then \
		echo "Error: No public key found in ~/.ssh/id_rsa.pub"; \
		exit 1; \
	fi
	$(Q)sed "s|{{SSH_PUBLIC_KEY}}|$(VM_SSH_PUBKEY)|g" $< > $@

$(VM_CACHE_DIR):
	$(Q)mkdir -p $@

$(VM_DIR):
	$(Q)mkdir -p $@

# download image file
# -------------------
$(VM_CACHE_FILE) : | $(VM_CACHE_DIR)
	$(Q)echo "[+] Downloading VM-DISK: $(REL_URL)"
	$(Q)wget -nv --no-verbose --show-progress -O $@.tmp $(REL_URL)
	$(Q)mv $@.tmp $@
	$(Q)chmod 444 $@

# copy disk image, rename and resize
# ----------------------------------
$(VM_DISK): | $(VM_CACHE_FILE) $(VM_DIR)
	$(Q)echo "[+] Creating VM-DISK: $@"
	$(Q)cp $(VM_CACHE_FILE) $@
	$(Q)chmod 644 $@
	$(Q)qemu-img resize -q $@  +100M
	$(Q)chmod 444 $@

.PHONY: vm-config
vm-config:
	@echo "-------SRC_IMAGE------------"
	@echo "OS_NAME=$(OS_NAME)"
	@echo "OS_VARIANT=$(OS_VARIANT)"
	@echo "REL_NAME=$(REL_NAME)"
	@echo "REL_FILE=$(REL_FILE)"
	@echo "REL_VER=$(REL_VER)"
	@echo "MIRROR=$(MIRROR)"
	@echo "REL_URL=$(REL_URL)"
	@echo "------INSTALL_IMAGE------------"
	@echo "CACHE_DIR=$(VM_CACHE_DIR)"
	@echo "VM_BASE_IMAGE=$(VM_CACHE_FILE)"
	@echo "VM_RUN_IMAGE=$(VM_DISK)"
	@echo "VM_USER_DATA=$(VM_USER_DATA)"
	@echo "VM_PUB_KEYFILE=$(VM_PUB_KEYFILE)"

.PHONY:vm-cache
vm-cache:
	ls -lh $(VM_CACHE_DIR)

# install vm image
# ----------------
.PHONY:vm-install
vm-install: $(VM_DISK) $(VM_USER_DATA)
	$(Q)echo "[+] Installing VM: $(VM_NAME)"
	$(Q)virt-install --quiet --noautoconsole --noreboot \
	--name $(VM_NAME) \
	--virt-type kvm \
	--ram 512 \
	--vcpus 1 \
	--disk path=$(VM_DISK),format=qcow2,bus=virtio \
	--network network=default,model=virtio \
	--cloud-init user-data=$(VM_USER_DATA) \
	--os-variant $(OS_VARIANT) \
	--graphics vnc \
	--rng /dev/urandom \
	--import
	$(Q)echo "[+] Started VM: $(VM_NAME)"

# ensure vm exists
# ----------------
$(VM_DONE): | $(BUILD_DIR)
	$(Q)virsh -q dominfo $(VM_NAME) >/dev/null 2>&1 || $(MAKE) vm-install
	$(Q)touch $@

.PHONY:vm-create
vm-create: $(VM_DONE)
	@$(MAKE) vm-start

.PHONY: vm-start
vm-start:
	$(Q)virsh -q list --state-running --name | grep -q "^$(VM_NAME)" || \
		(echo "[+] Starting VM: $(VM_NAME)" && virsh -q start $(VM_NAME))
	$(Q)$(MAKE) vm-wait

# wait for ssh access
# -------------------
VM_WAIT_RETRIES = 30
VM_WAIT_SLEEP   = 3
VM_WAIT_TIMEOUT = $(shell expr $(VM_WAIT_RETRIES) \* $(VM_WAIT_SLEEP))
.PHONY:vm-wait
vm-wait:
	$(Q)echo "[+] Waiting for VM $(VM_NAME) to reach SSH"
	@count=0; \
	while [ $$count -lt $(VM_WAIT_RETRIES) ]; do \
		VM_IP=$$(virsh -q domifaddr $(VM_NAME) --source lease | awk '{print $$4}' | cut -d/ -f1); \
		if [ -n "$$VM_IP" ] && nc -z -w 2 $$VM_IP 22 >/dev/null 2>&1; then \
			echo " => VM is UP at $$VM_IP."; \
			exit 0; \
		fi; \
		sleep $(VM_WAIT_SLEEP); \
		count=$$((count + 1)); \
		echo " ... still waiting ($$count/$(VM_WAIT_RETRIES))"; \
	done; \
	echo " [ERROR] VM failed to reach SSH after $(VM_WAIT_TIMEOUT) seconds"; \
	exit 1

.PHONY: vm-list
vm-list:
	@echo "[+] Checking VM $(VM_NAME)"
	$(Q)virsh -q dominfo $(VM_NAME)   2>/dev/null || echo " => VM not found"
	$(Q)virsh -q domifaddr $(VM_NAME) 2>/dev/null || true

vm-clean:
	 - virsh -q destroy $(VM_NAME)
	 - virsh -q undefine $(VM_NAME)
	 - rm -rf $(VM_DIR) $(VM_DONE) $(VM_USER_DATA)

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
K8S_FILES = k8s/client.yaml k8s/database.yaml k8s/network-policy.yaml k8s/kustomization.yaml

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
	k3d cluster list --no-headers | awk '{print $$1}' | grep -qx "db-k8s" || k3d cluster create db-k8s
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
$(DEPLOY_DONE): $(LOAD_DONE) $(K8S_FILES) | $(BUILD_DIR)
	@echo "Applying k8s manifest..."
	kubectl apply -k k8s/
	@echo "Restarting pods..."
	kubectl rollout restart deployment/client-app
	kubectl rollout restart statefulset/server-pod
	touch $(DEPLOY_DONE)

# misc k8s commands
# -----------------
.PHONY: list-cluster
list-cluster:
	k3d cluster list

.PHONY: apply
apply:
	kubectl apply -k k8s/

.PHONY: list-pod
list-pod:
	@printf '%66s\n' | tr ' ' '-'
	@kubectl get statefulset/server-pod deployment/client-app
	@printf '%66s\n' | tr ' ' '-'
	@kubectl get pods -l 'app in (client-pod,db-pod)'

.PHONY: list-net
list-net:
	@printf '%66s\n' | tr ' ' '-'
	kubectl describe networkpolicy server-protection
	@printf '%66s\n' | tr ' ' '-'
	kubectl describe networkpolicy client-protection

.PHONY: list-all
list-all:
	@printf '%66s\n' | tr ' ' '-'
	kubectl get all

.PHONY: show-log
show-log:
	kubectl logs statefulset/server-pod | tail -n 20
	kubectl logs deployment/client-app  | tail -n 20

# kubectl logs -l app=client-pod -f --prefix
# kubectl logs -l app=db-pod -f --prefix

# ###########################
# TEST SUITE MACROS & TARGETS
# ###########################

# colors
# ------
ifneq ($(MAKE_TERMOUT),)
    # safe to use colors escape codes
    GREEN  := $(shell tput setaf 2)
    RED    := $(shell tput setaf 1)
    RESET  := $(shell tput sgr0)
	CHECK_MARK := $(shell printf "\342\234\223")
	CROSS_MARK := $(shell printf "\342\234\227")
	ROCKET := 🚀
	CHECK  := ✅
  	CROSS  := ❌
	BULB   := 💡 
else
    # not safe (redirected to a file)
    GREEN  :=
    RED    :=
    RESET  :=
	CHECK_MARK :=
	CROSS_MARK :=
	ROCKET := [LAUNCH]
	CHECK  := [OK]
	CROSS  := [FAIL]
	BULB   := [HINT]
endif

PASS_STR := [$(GREEN) PASS $(RESET)]
FAIL_STR := [$(RED) FAIL $(RESET)]

ALLOW_STR := $(CHECK_MARK) ALLOWED
DENY_STR  := $(CROSS_MARK) DENIED

DB_PORT = 6379
TEST_PORT = 7379
TEST_ADDR = 127.0.0.1
CMD_ARGS = --hostname $(TEST_ADDR) --port $(TEST_PORT)

TEST_LOGFILE  = $(BUILD_DIR)/test.log
TEST_REQFILE = tests/test_req.txt
TEST_RSPFILE = tests/test_rsp.txt
RES_REQFILE  = $(BUILD_DIR)/test_req.txt
RES_RSPFILE  = $(BUILD_DIR)/test_rsp.txt
TEST_WAITRUN = 0.5

TEST_REPORT = \
	passed=$$((total - errors)); \
	[ $$total -eq 0 ] && percent=100 || percent=$$(( (total - errors) * 100 / total )); \
	echo " => Ran $$total tests: $$passed passed, $$errors failed ($$percent% success)"

# 1=log-file 2=out-file
GET_REQRSP = sed -e 's/^> //' -e '/^\[+]/d' $(1) > $(2)

# test-cmd macros
# ---------------

# 1=pid
SHUTDOWN_PID = \
	echo " => Shutting down PID $(1)"; \
	kill $(1) 2>/dev/null; \
	wait $(1) 2>/dev/null || true

# 1=pid 2=name
WAIT_PIDUP= \
	echo " => Starting $(2) PID $(1)"; \
	sleep $(TEST_WAITRUN); \
	kill -0 $(1); \
	if [ $$? -ne 0 ]; then \
		echo " => $(2) died - check log"; \
		exit 1; \
	fi

# 1=addr 2=port 3=timeout pid=4
WAIT_CONNUP = \
	echo " => Waiting for $(1):$(2)"; \
	timeout $(3) bash -c 'until nc -z $(1) $(2) 2>/dev/null; do sleep 0.1; done'; \
	if [ $$? -ne 0 ]; then \
		 echo " => Wait failed to connect"; \
		 $(call SHUTDOWN_PID,$(4)); \
		 exit 1; \
	fi; \
	echo " => Connection is UP - starting tests"

# 1=test-file 2=res-file 3=log-file
DIFF_FILE = \
	total=$$((total + 1));  \
	diff -q --strip-trailing-cr $(1) $(2) >> $(3); \
    if [ $$? -eq 0 ]; then \
        printf " => TEST %s %s\n" "$(1)" "$(PASS_STR)"; \
    else \
        printf " => TEST '%s' %s\n" "$(1)" "$(FAIL_STR)"; \
        errors=$$((errors + 1)); \
    fi

# test-server macros
# ------------------
CHK_SRVCMD = \
    total=$$((total + 1)); \
    echo "$(1)" | nc -w 1 -N $(TEST_ADDR) $(TEST_PORT) | grep -q "$$EXPECT"; \
    if [ $$? -eq 0 ]; then \
        printf " => TEST '%s' %s\n" "$(1)" "$(PASS_STR)"; \
    else \
        printf " => TEST '%s' %s\n" "$(1)" "$(FAIL_STR)"; \
        errors=$$((errors + 1)); \
    fi

# test-client macros
# ------------------
CLI_LOGFILE  = $(BUILD_DIR)/test_client.log
SIMPLE_SERVER = scripts/simple_server.awk

# test-lau macros
# ---------------
# ah lau we hardly knew ye
LAU_LOGFILE = $(BUILD_DIR)/test_lau.log
LAU_RESFILE = $(BUILD_DIR)/test_lau.rsp
LAU_BIN := $(VM_BIN_DIR)/launcher
LAU_CMD := doas $(LAU_BIN) --base-dir $(VM_HOME)/$(VM_NAME) --src-dir $(VM_BIN_DIR)

# 1=vm-name
VM_GET_IP = virsh -q domifaddr $(VM_NAME) --source lease | awk '{print $$4}' | cut -d/ -f1

# allow ssh be run without user input
SSH_OPTS = \
	-o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null \
	-o LogLevel=ERROR \
	-o IdentitiesOnly=yes -i $(VM_SSH_KEYFILE)
ifeq ($(V),1)
  SSH_OPTS += -v
endif

# 1=cmd-file
define SEND_CMDS
	( \
	  sleep 1.5; \
	  while IFS= read -r line; do \
	    echo "$$line"; \
	    sleep 0.3; \
	  done < $(1) \
	)
endef

# test-pod / test-net 
# ---------------------
CLIENT_POD := client-pod
DB_POD := db-pod

# test-pod macros
# ---------------
TEST_POD_LOGFILE = $(BUILD_DIR)/test_pod.log
TEST_POD_RESFILE = $(TEST_POD_LOGFILE).res

# 1=file
GREP_CONNOK = grep -Fc "[+] Connectivity test: OK" $(1) | wc -l
# 1=req, 2=rsp, 3=file
GREP_REQRSP  = grep -Fq "[LOG] send req: $(1) recv rsp: $(2)" $(3)
# 1=cmd,2=file
SND_PODCMD = echo $(1) | kubectl attach -qi $(2) 2>/dev/null
# 1=pod
GET_PODS = kubectl get pods -l app=$(1) -o name
GET_POD  = kubectl get pods -l app=$(1) -o name | head -n 1
# 1=pod 2=file
GET_LOGS = \
	POD_LIST=$$($(call GET_PODS,$(1))); \
	true > $(2); \
	for POD in $$POD_LIST; do \
		kubectl logs $$POD >$(2) 2>/dev/null; \
	done
# 1=pod 2=log
GET_LOGFILE  = kubectl logs $(1) --tail=20 >$(2) 2>/dev/null
# 1=log 2=file
GET_RESFILE = sed -e 's/^> //' -e '/^\[LOG\]/!d' $(1) | \
	sed -z 's/\n\[LOG\] recv rsp:/ recv rsp:/g' > $(2)
# 1=cmd 2=res 3=file
CHK_RESFILE = \
	total=$$((total + 1)); \
	$$($(call $(GREP_REQRSP) $(1) $(2) $(3))); \
	if [ $$? -eq 0 ]; then \
		printf " => check %s %b\n" "$(1)" "$(PASS_STR)"; \
	else \
		printf " => check %s %b\n" $(1) "$(FAIL_STR)"; \
		errors=$$((errors + 1)); \
	fi

# test-net macros
# ---------------
INET_HOST = google.com
INET_PORT = 443
INET_NAME = internet
DB_NAME   = $(DB_POD):$(DB_PORT)
RANDOM_NAME = random
RANDOM_POD = random-pod

# 1=name 2=label 3=timeout
TEST_RUNPOD = \
	kubectl run $(1) --image=busybox -l app=$(2) --restart=Never -- sleep 30 >/dev/null 2>&1; \
	kubectl wait --for=condition=Ready pod/$(1) --timeout=$(3) >/dev/null 2>&1
# 1=name
TEST_DELPOD = kubectl delete pod $(1) --now >/dev/null 2>&1
# 1=pod 2=name
LOG_CHKCONN = printf " => %-10s -> %-15s " $(1) $(2)
# 1=is_open 2=pod 3=label 4=hostname 5=port 6=log_chk
CHK_CONNOPEN = \
	log_chk=$(if $(6),$(6),1); \
	if [ $$log_chk -eq 1 ]; then \
		$(call LOG_CHKCONN,$(2),$(3)); \
	fi; \
	total=$$((total + 1)); \
	POD=$$($(call GET_POD,$(2))); \
	kubectl exec $$POD -- nc -w 3 -zv $(4) $(5) >/dev/null 2>&1; \
	if [ $$? -eq 0 ]; then is_open=1; else is_open=0; fi; \
	if [ $(1) -eq 0 ]; then perm_str="$(DENY_STR)"; else perm_str="$(ALLOW_STR)"; fi; \
	if [ $$is_open -eq $(1) ]; then \
		printf "%b %b\n" "$$perm_str" "$(PASS_STR)"; \
	else \
		printf "%b %b\n" "$$perm_str" "$(FAIL_STR)"; \
		errors=$$((errors + 1)); \
	fi
# 1=is_open 2=pod 3=label 4=hostname 5=port
CHK_RANDOPEN= \
	$(call LOG_CHKCONN,$(2),$(3)); \
	$(call TEST_RUNPOD,$(2),$(RANDOM_NAME),15s); \
	$(call CHK_CONNOPEN,$(1),$(2),$(3),$(4),$(5),0); \
	$(call TEST_DELPOD,$(2))

.PHONY: test-full
test-full: test-cmds test-lau test-k8s
	@echo "$(ROCKET) Full test suite complete."

.PHONY: test
test: test-cmds
	@echo "-------------------------------------------------------"
	@echo "$(BULB) Next step: 'make test-full' for VM/K8s tests."

# test cmds (client|server)
# ------------------------
.PHONY: test-cmds
test-cmds: test-server test-client
	@echo "$(CHECK) $@ complete."

# test ./server
# -----------------
.PHONY: test-server
test-server: server
	$(Q)echo "[+] Running $@"; \
	./server $(CMD_ARGS) 1> $(TEST_LOGFILE) 2>&1 & SRV_PID=$$!; \
	$(call WAIT_PIDUP,$$SRV_PID,server); \
	$(call WAIT_CONNUP,$(TEST_ADDR),$(TEST_PORT),$$SRV_PID); \
	total=0; errors=0; \
	$(call CHK_SRVCMD,SET foo bar,OK); \
	$(call CHK_SRVCMD,GET foo,bar); \
	$(call CHK_SRVCMD,DEL foo,OK); \
	$(call CHK_SRVCMD,GET foo,FAIL); \
	$(call CHK_SRVCMD,SET key value1,OK); \
	$(call CHK_SRVCMD,GET key,value1); \
	$(call CHK_SRVCMD,SET key value2, OK); \
	$(call CHK_SRVCMD,GET key,value2); \
	$(call SHUTDOWN_PID,$$SRV_PID); \
	$(call TEST_REPORT); \
	if [ $$errors -gt 0 ]; then exit 1; fi

# test ./client 
# ----------------
.PHONY: test-client
test-client: client
	$(Q)echo "[+] Running $@"; \
	awk -f ./$(SIMPLE_SERVER) -v Port="$(TEST_PORT)" -v RespFile="$(TEST_RSPFILE)" 1>$(RES_REQFILE) 2>&1 & SRV_PID=$$!; \
	$(call WAIT_PIDUP,$$SRV_PID,simple-server); \
	$(call WAIT_CONNUP,$(TEST_ADDR),$(TEST_PORT),$$SRV_PID); \
	echo " => send cmd-file to client"; \
	cat $(TEST_REQFILE) | timeout 2s ./client $(CMD_ARGS) >$(CLI_LOGFILE) 2>&1; \
	sed -e 's/^> //' -e '/^\[+]/d' $(CLI_LOGFILE) > $(RES_RSPFILE); \
	$(call SHUTDOWN_PID,$$SRV_PID); \
	total=0; errors=0; \
	$(call DIFF_FILE,$(TEST_REQFILE),$(RES_REQFILE),$(CLI_LOGFILE)); \
	$(call DIFF_FILE,$(TEST_RSPFILE),$(RES_RSPFILE),$(CLI_LOGFILE)); \
	$(call TEST_REPORT); \
	if [ $$errors -gt 0 ]; then exit 1; fi

# run launcher tests
# ------------------
.PHONY: test-lau
test-lau: $(INSTALL_DONE) vm-create
	$(Q)echo "[+] Running $@"; \
	VM_IP=$$($(VM_GET_IP)); \
	if [ -z "$$VM_IP" ]; then echo "[ERROR] No VM ip address"; exit 1; fi; \
	VM_SSH_ADDR="$(VM_USER)@$$VM_IP"; \
	echo " => Copying $(BIN_DIR) to $$VM_SSH_ADDR:$(VM_HOME)"; \
	scp -q $(SSH_OPTS) -r $(BIN_DIR) $$VM_SSH_ADDR:$(VM_HOME); \
	echo " => Sending cmds to $(TEST_LAU) ..."; \
	$(call SEND_CMDS,$(TEST_REQFILE)) | ssh -tt $(SSH_OPTS) $$VM_SSH_ADDR "$(LAU_CMD)" 2>&1 | tr -d '\r' | tee $(LAU_LOGFILE); \
	echo " => Fetching logs"; \
	sed -n -u -e 's/^> //p'  $(LAU_LOGFILE) >$(RES_REQFILE); \
	sed -n -u -e '/^[A-Za-z0-9]/p' $(LAU_LOGFILE) >$(RES_RSPFILE); \
	total=0; errors=0; \
	$(call DIFF_FILE,$(TEST_REQFILE),$(RES_REQFILE),$(LAU_LOGFILE)); \
	$(call DIFF_FILE,$(TEST_RSPFILE),$(RES_RSPFILE),$(LAU_LOGFILE)); \
	$(call TEST_REPORT); \
	if [ $$errors -gt 0 ]; then exit 1; fi

# run all k8s tests
# -----------------
.PHONY: test-k8s 
test-k8s:wait-pods test-pod test-net
	@echo "$(CHECK) $@ complete."

# wait for pods to be ready
# -------------------------
.PHONY:wait-pods
wait-pods: deploy
	$(Q)echo "[+] Running $@"
	$(Q)echo " => kubectl wait for $(DB_POD) ready ..."
	$(Q)kubectl wait --for=condition=Ready pod -l app=$(DB_POD) --timeout=30s | sed 's/^/ => /'
	$(Q)echo " => kubectl wait for $(CLIENT_POD) ready ..."
	$(Q)kubectl wait --for=condition=Ready pod -l app=$(CLIENT_POD) --timeout=30s | sed 's/^/ => /'
	$(Q)NEED_OK=1; NUM_OK=0; \
	echo " => Wait for $$NEED_OK $(CLIENT_POD) connected ..."; \
	for i in { 1..3}; do \
		$(call GET_LOGS,$(CLIENT_POD),$(TEST_POD_LOGFILE)); \
		NUM_OK=$$($(call GREP_CONNOK,$(TEST_POD_LOGFILE))); \
		if [ "$$NUM_OK" -ge "$$NEED_OK" ]; then break; fi; \
		sleep 2; \
	done; \
	if [ "$$NUM_OK" -lt "$$NEED_OK" ]; then exit 1; fi; \
	echo " => $(CHECK_MARK) $@ complete."

# test SET|GET|DEL via client-pods
# --------------------------------
.PHONY: test-pod
test-pod: deploy
	$(Q)echo "[+] Running $@"; \
	POD=$$($(call GET_POD,$(CLIENT_POD))); \
	echo " => Using $(CLIENT_POD): $$POD"; \
	RAND_STR=$$(LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 30); \
	CMD1="SET test-pod $$RAND_STR"; \
	CMD2="GET test-pod"; \
	CMD3="DEL test-pod"; \
	echo " => Sending cmds"; \
	$(call SND_PODCMD,$$CMD1,$$POD); \
	$(call SND_PODCMD,$$CMD2,$$POD); \
	$(call SND_PODCMD,$$CMD3,$$POD); \
	echo " => Fetching logs"; \
	$(call GET_LOGFILE,$$CLIENT_POD,$(TEST_POD_LOGFILE)); \
	$(call GET_RESFILE,$(TEST_POD_LOGFILE),$(TEST_POD_RESFILE)); \
	echo " => Checking results"; \
	total=0; errors=0; \
	$(call CHK_RESFILE,$$CMD1,OK,$(TEST_POD_RESFILE)); \
	$(call CHK_RESFILE,$$CMD2,$$RAND_STR,$(TEST_POD_RESFILE)); \
	$(call CHK_RESFILE,$$CMD3,OK,$(TEST_POD_RESFILE)); \
	$(call TEST_REPORT); \
	if [ $$errors -gt 0 ]; then exit 1; fi

# test k8s Network Policy
# -----------------------
.PHONY: test-net
test-net: deploy
	$(Q)echo "[+] Running $@"; \
	total=0; errors=0; \
	$(call CHK_CONNOPEN,0,$(DB_POD),$(INET_NAME),$(INET_HOST),$(INET_PORT)); \
	$(call CHK_CONNOPEN,0,$(CLIENT_POD),$(INET_NAME),$(INET_HOST),$(INET_PORT)); \
	$(call CHK_CONNOPEN,1,$(CLIENT_POD),$(DB_NAME),db-service,$(DB_PORT)); \
	$(call CHK_RANDOPEN,0,$(RANDOM_POD),$(DB_NAME),db-service,$(DB_PORT)); \
	$(call TEST_REPORT); \
	if [ $$errors -gt 0 ]; then exit 1; fi

# ################
# --- Cleanup ----
# ################

# delete cluster and docker images
# -------------------------------
.PHONY: clean-k8s
clean-k8s:
	@echo "Cleaning k8s config"
	k3d cluster delete $(CLUSTER_NAME) || true
	docker rmi $(SERVER_IMG) $(CLIENT_IMG) || true
	rm -f $(DONE_FILES)

.PHONY: clean-rootfs
clean-rootfs:
	rm -rf $(ROOTFS_DIR) $(ROOTFS_DONE)

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(CMDS) $(BIN_DIR) tags

.PHONY: clean-all
clean-all: clean-k8s clean-rootfs vm-clean clean
	@echo "Clean done"

.PHONY: spotless
spotless: clean-all
	@echo "Wiping everthing"
	-docker system prune -af --volumes

