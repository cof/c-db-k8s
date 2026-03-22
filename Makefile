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
#  test-all    : build and test everything
#  test-cmds   : build and test cmds (client|server)
#  test-server : build and test ./server
#  test-client : build and test ./client
#  test-lau    : build and test launcher
#
#  test-k8s    : do all these tests
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
GCC_DEPS     := -MMD -MP
CPP_FLAGS    := -D_GNU_SOURCE -Isrc
EXTRA_CFLAGS := -Wextra -Wno-missing-field-initializers
DEBUG_COMMON := -ggdb3 -fsanitize=address -fno-omit-frame-pointer
DEBUG_CFLAGS := -O0 $(EXTRA_CFLAGS)
RELEASE_CFLAGS := -O2 $(EXTRA_CFLAGS)
CFLAGS += -Wall $(CPP_FLAGS) $(GCC_DEPS)

DEBUG ?= 0
ifeq ($(DEBUG), 1)
	CFLAGS  += $(DEBUG_CFLAGS) $(DEBUG_COMMON)
	LDFLAGS += $(DEBUG_COMMOM)
else
	CFLAGS += $(RELEASE_CFLAGS)
	LDFLAGS += --static
endif

MAKEFLAGS += --no-print-directory

# ###########################
# CMDS server|client|launcher
# ###########################

# Default target - build cmds
# --------------------------
.PHONY: all
all: $(CMDS) | $(BUILD_DIR)

$(BUILD_DIR):
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
	@strace -c -f -o $(STRACE_SRV) ./server $(TEST_ARGS) & STRACE_PID=$$!; \
	SERV_PID=$$(pgrep -P $$STRACE_PID); \
	echo "Profiling Server (PID: $$SERV_PID) via Strace (PID: $$STRACE_PID)"; \
	sleep 1; \
	strace -c -f -o $(STRACE_CLI) ./client $(TEST_ARGS) < $(CMD_FILE) 1>/dev/null || true; \
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
VM_NAME := test-lau
VM_FILE := myalpine.qcow2
VM_DIR  := vmdir
VM_USER := alpine
VM_HOME := /home/$(VM_USER)
VM_BIN_DIR  := $(VM_HOME)/bin

# where we store downloads
ifeq ($(origin VM_CACHE_DIR), undefined)
  VM_CACHE_DIR := $(shell echo $${XDG_CACHE_HOME:-$$HOME/.cache}/my-vm-project)
endif

VM_CACHE_FILE = $(VM_CACHE_DIR)/$(REL_FILE)
VM_DISK = $(VM_DIR)/$(VM_FILE)
#VM_MAC := 52:54:00:12:34:56
#VM_IP  := 192.168.122.243

# alpine VMs  use user-data.yaml to autoconfigure
USER_DATA = $(BUILD_DIR)/user-data.yaml
VM_DONE   = $(BUILD_DIR)/.vm_done

# get public key
SSH_PUB_KEY := $(shell cat ~/.ssh/id_rsa.pub 2>/dev/null || echo "NO_KEY_FOUND")

# create cloud-init user-data
$(USER_DATA): tests/user-data.yaml | $(BUILD_DIR)
	$(Q)if [ "$(SSH_PUB_KEY)" = "NO_KEY_FOUND" ]; then \
		echo "Error: No public key found in ~/.ssh/id_rsa.pub"; \
		exit 1; \
	fi
	$(Q)sed "s|{{SSH_PUBLIC_KEY}}|$(SSH_PUB_KEY)|g" $< > $@

$(VM_CACHE_DIR):
	$(Q)mkdir -p $@

$(VM_DIR):
	$(Q)mkdir -p $@

# download image file
$(VM_CACHE_FILE) : | $(VM_CACHE_DIR)
	$(Q)echo "[+] Downloading VM-DISK: $(REL_URL)"
	$(Q)wget -nv --no-verbose --show-progress -O $@.tmp $(REL_URL)
	$(Q)mv $@.tmp $@
	$(Q)chmod 444 $@

# create renamed|resized copy
$(VM_DISK): | $(VM_CACHE_FILE) $(VM_DIR)
	$(Q)echo "[+] Creating VM-DISK: $@"
	$(Q)cp $(VM_CACHE_FILE) $@
	$(Q)chmod 644 $@
	$(Q)qemu-img resize -q $@  +100M
	$(Q)chmod 444 $@

.PHONY: vm-config
vm-config:
	@echo "MIRROR=$(MIRROR)"
	@echo "REL_URL=$(REL_URL)"
	@echo "REL_FILE=$(REL_FILE)"
	@echo "REL_VER=$(REL_VER)"
	@echo "CACHE_DIR=$(VM_CACHE_DIR)"
	@echo "BASE_IMAGE=$(VM_CACHE_FILE)"
	@echo "RUN_IMAGE=$(VM_DISK)"

.PHONY:vm-cache
vm-cache:
	ls -lh $(VM_CACHE_DIR)

.PHONY:vm-install
vm-install: $(VM_DISK) $(USER_DATA)
	$(Q)echo "[+] Installing VM: $(VM_NAME)"
	$(Q)virt-install --quiet --noautoconsole --noreboot \
	--name $(VM_NAME) \
	--virt-type kvm \
	--ram 512 \
	--vcpus 1 \
	--disk path=$(VM_DISK),format=qcow2,bus=virtio \
	--network network=default,model=virtio \
	--cloud-init user-data=$(USER_DATA) \
	--os-variant $(OS_VARIANT) \
	--graphics vnc \
	--rng /dev/urandom \
	--import
	$(Q)echo "[+] Started VM: $(VM_NAME)"

# ensure vm exists
# ----------------
$(VM_DONE): | $(BUILD_DIR)
	$(Q)virsh dominfo -q $(VM_NAME) >/dev/null 2>&1 || $(MAKE) vm-install
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
	$(Q)echo " => Waiting for VM $(VM_NAME) to reach SSH"
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
	virsh dominfo $(VM_NAME) || true
	virsh domifaddr $(VM_NAME) || true

vm-clean:
	 - virsh -q destroy $(VM_NAME)
	 - virsh -q undefine $(VM_NAME)
	 - rm -rf $(VM_DIR) $(VM_DONE)

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
$(DEPLOY_DONE): $(LOAD_DONE) | $(BUILD_DIR)
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
ifneq ($(MAKE_TERMOUT),)
    # safe to use colors escape codes
    GREEN  := $(shell tput setaf 2)
    RED    := $(shell tput setaf 1)
    RESET  := $(shell tput sgr0)
	CHECK  := $(shell printf "\342\234\223")
	CROSS  := $(shell printf "\342\234\227")
else
    # not safe (redirected to a file)
    GREEN  :=
    RED    :=
    RESET  :=
	CHECK  :=
	CROSS  :=
endif

PASS_STR := [$(GREEN) PASS $(RESET)]
FAIL_STR := [$(RED) FAIL $(RESET)]

ALLOW_STR := $(CHECK) ALLOWED
DENY_STR  := $(CROSS) DENIED

SERV_PORT = 6379
TEST_PORT = 7379
TEST_ADDR = 127.0.0.1
TEST_ARGS = --hostname $(TEST_ADDR) --port $(TEST_PORT)
TEST_LOG  = $(BUILD_DIR)/test.log
TEST_WAIT_RUN = 0.5

TEST_REQ_FILE = tests/test_req.txt
TEST_RSP_FILE = tests/test_rsp.txt

SSH_OPTS = -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
GET_VM_IP = virsh -q domifaddr $(VM_NAME) --source lease | awk '{print $$4}' | cut -d/ -f1
WAIT_UP   = timeout $(1) bash -c 'until nc -z $(2) $(3) 2>/dev/null; do sleep 0.1; done'
KILL_WAIT = kill $(1) 2>/dev/null;  wait $(1) 2>/dev/null || true

TEST_CMD = \
    total=$$((total + 1)); \
    echo "$(1)" | nc -w 1 -N $(TEST_ADDR) $(TEST_PORT) | grep -q "$$EXPECT"; \
    if [ $$? -eq 0 ]; then \
        printf " => TEST '%s' %s\n" "$(1)" "$(PASS_STR)"; \
    else \
        printf " => TEST '%s' %s\n" "$(1)" "$(FAIL_STR)"; \
        errors=$$((errors + 1)); \
    fi

TEST_FILE = \
	total=$$((total + 1));  \
	diff --strip-trailing-cr $(1) $(2) 1>>$(TEST_LOG); \
    if [ $$? -eq 0 ]; then \
        printf " => TEST %s %s\n" "$(1)" "$(PASS_STR)"; \
    else \
        printf " => TEST '%s' %s\n" "$(1)" "$(FAIL_STR)"; \
        errors=$$((errors + 1)); \
    fi

TEST_REPORT = \
	passed=$$((total - errors)); \
	[ $$total -eq 0 ] && percent=100 || percent=$$(( (total - errors) * 100 / total )); \
	echo " => Ran $$total tests: $$passed passed, $$errors failed ($$percent% success)"

BUILD_REQ_FILE = $(BUILD_DIR)/$(notdir $(TEST_REQ_FILE))
BUILD_RSP_FILE = $(BUILD_DIR)/$(notdir $(TEST_RSP_FILE))
SIMPLE_SERVER = scripts/simple_server.awk

# TODO parse k8s/yaml files to get names
CLIENT_POD := client-pod
DB_POD := db-pod

REQ_START := $(subst ",,"[LOG] send req: ")
RSP_START := $(subst ",," recv rsp: ")
CLIENT_OK := $(subst ",,"Connectivity test: OK")

TEST_POD_LOG = $(BUILD_DIR)/testpod.txt
TEST_POD_RES = $(TEST_POD_LOG).result
GET_APP    = kubectl get pods -l app=$(1) -o name | head -n 1
SND_ATTACH = echo $(1) | kubectl attach -qi $(2) 2>/dev/null
GET_LOGS = kubectl logs $(1) --tail=20 >$(2) 2>/dev/null
DO_CLEAN = sed -e 's/^> //' -e '/^\[LOG\]/!d' $(1) | \
	sed -z 's/\n\[LOG\] recv rsp:/ recv rsp:/g' > $(2)

TEST_RESULT = \
	total=$$((total + 1));  \
	grep -Fq "$(REQ_START)$(1)$(RSP_START)$(2)" $(3); \
	if [ $$? -eq 0 ]; then \
		printf " => check %s %b\n" "$(1)" "$(PASS_STR)"; \
	else \
		printf " => check %s %b\n" $(1) "$(FAIL_STR)"; \
		errors=$$((errors + 1)); \
	fi

TEST_CONNECT = \
	total=$$((total + 1));  \
	kubectl exec $(2) -- nc -w 3 -zv $(3) $(4) >/dev/null 2>&1; \
	exit_code=$$?; [ $$exit_code -ne 0 ] && exit_code=1; \
	perm_str="$(ALLOW_STR)"; [ $(1) -ne 0 ] && perm_str="$(DENY_STR)"; \
	if [ $$exit_code -eq $(1) ]; then \
		printf "%b %b\n" "$$perm_str" "$(PASS_STR)"; \
	else \
		printf "%b %b\n" "$$perm_str" "$(FAIL_STR)"; \
		errors=$$((errors + 1)); \
	fi

.PHONY: test
test: test-cmds test-k8s

# test cmds (client|server) TODO launcher - need VM)
# -------------------------------------------------
.PHONY: test-cmds
test-cmds: test-server test-client

# test ./server
# -----------------
.PHONY: test-server
test-server: server
	$(Q)echo "[+] Running $@"; \
	./server $(TEST_ARGS) 1> $(TEST_LOG) 2>&1 & SRV_PID=$$!; \
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
		 1>$(TEST_LOG) 2>&1 & SRV_PID=$$!; \
	echo " => Starting simple-server PID $$SRV_PID"; \
	sleep $(TEST_WAIT_RUN); \
	kill -0 $$SRV_PID || { echo " => simple-server died - check log"; exit 1; }; \
	echo " => Waiting for $(TEST_ADDR):$(TEST_PORT)"; \
	$(call WAIT_UP,3,$(TEST_ADDR),$(TEST_PORT)) || \
		{ echo " => Wait failed";i $(call KILL_WAIT,$$SRV_PID); exit 1; }; \
	echo " => Server is UP send $(TEST_REQ_FILE) via client"; \
	cat $(TEST_REQ_FILE) | timeout 2s ./client $(TEST_ARGS) > $(BUILD_RSP_FILE); \
	sed -i -e 's/^> //' -e '/^\[+]/d' $(BUILD_RSP_FILE); \
	echo " => Shutting down simple-server PID $$SRV_PID"; \
	$(call KILL_WAIT,$$SRV_PID); \
	total=0; errors=0; \
	$(call TEST_FILE,$(TEST_REQ_FILE),$(BUILD_REQ_FILE)); \
	$(call TEST_FILE,$(TEST_RSP_FILE),$(BUILD_RSP_FILE)); \
	$(call TEST_REPORT); \
	if [ $$errors -gt 0 ]; then exit 1; fi

# test-lau
# --------------
.PHONY: test-lau
test-lau: $(INSTALL_DONE) vm-create
	$(Q)echo "Running $@"; \
	> $(TEST_LOG); \
	VM_IP=$$($(GET_VM_IP)); \
	if [ -z "$$VM_IP" ]; then echo "[ERROR] No VM ip address"; exit 1; fi; \
	VM_SSH_ADDR="$(VM_USER)@$$VM_IP"; \
	echo "Copying $(BIN_DIR) to $$VM_SSH_ADDR:$(VM_BIN_DIR)"; \
	scp $(SSH_OPTS) -r $(BIN_DIR)/* $$VM_SSH_ADDR:$(VM_BIN_DIR); \
	echo "=> Verifying loader..."; \
	ssh $(SSH_OPTS) -tt $$VM_SSH_ADDR "stty -echo; sudo $(VM_BIN_DIR)/launcher --base-dir $(VM_HOME)/$@ --src-dir $(VM_BIN_DIR)" < ./$(TEST_REQ_FILE)

.PHONY: test-k8s 
test-k8s:wait-pods test-pod test-net

# wait for all pods to be ready
# ----------------------------
.PHONY:wait-pods
wait-pods: deploy
	$(Q)echo "[+] Waiting for cluster to be READY..."
	$(Q)echo " => kubectl waiting for $(DB_POD) ..."
	$(Q)kubectl wait --for=condition=Ready pod -l app=$(DB_POD) --timeout=30s | sed 's/^/ => /'
	$(Q)echo " => kubectl waiting for $(CLIENT_POD)s ..."
	$(Q)kubectl wait --for=condition=Ready pod -l app=$(CLIENT_POD) --timeout=30s | sed 's/^/ => /'
	$(Q)echo "[+] Waiting for $(CLIENT_OK)"; \
	count=0; \
	until kubectl logs -l app=client-pod --tail=10 2>/dev/null | grep -Fq "$(CLIENT_OK)"; \
	do \
		if [ $$count -eq 3 ]; then \
			echo " => [ERROR} timeout waiting for $(CLIENT_OK) in logs"; \
			exit 1; \
		fi; \
		printf "."; \
		sleep 1; \
		count=$$((count + 1)); \
	done; \
	echo "[+} Pods ready"

# test SET|GET|DEL via client-pods
# --------------------------------
.PHONY: test-pod
test-pod: deploy
	$(Q)echo "[+] Running $@"; \
	CLIENT_POD=$$($(call GET_APP,$(CLIENT_POD))); \
	echo " => Using $(CLIENT_POD): $$CLIENT_POD"; \
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
	total=0; errors=0; \
	$(call TEST_RESULT,$$CMD1,OK,$(TEST_POD_RES)); \
	$(call TEST_RESULT,$$CMD2,$$RAND_STR,$(TEST_POD_RES)); \
	$(call TEST_RESULT,$$CMD3,OK,$(TEST_POD_RES)); \
	$(call TEST_REPORT); \
	if [ $$errors -gt 0 ]; then exit 1; fi

# test k8s Network Policy
# -----------------------
.PHONY: test-net
test-net: deploy
	$(Q)echo "[+] Runing $@"; \
	total=0; errors=0; \
	echo -n " => Checking $(DB_POD) -> internet "; \
	POD=$$($(call GET_APP,$(DB_POD))); \
	$(call TEST_CONNECT,1,$$POD,google.com,443); \
	echo -n " => Checking $(CLIENT_POD) -> internet "; \
	POD=$$($(call GET_APP,$(CLIENT_POD))); \
	$(call TEST_CONNECT,1, $$POD,google.com,443); \
	echo -n " => Checking $(CLIENT_POD) -> $(DB_POD) "; \
	POD=$$($(call GET_APP,$(CLIENT_POD))); \
	$(call TEST_CONNECT,0,$$POD,db-service,$(SERV_PORT)); \
	echo -n " => Checking random-pod -> $(DB_POD):$(SERV_PORT) "; \
	kubectl run random-pod --image=busybox -l app=random --restart=Never -- sleep 30 >/dev/null 2>&1; \
	kubectl wait --for=condition=Ready pod/random-pod --timeout=15s >/dev/null 2>&1; \
	$(call TEST_CONNECT,1,random-pod,db-service,$(SERV_PORT)); \
	kubectl delete pod random-pod --now >/dev/null 2>&1; \
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
	-k3d cluster delete $(CLUSTER_NAME)
	-docker rmi $(SERVER_IMG) $(CLIENT_IMG)
	-rm -f $(DONE_FILES)

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

