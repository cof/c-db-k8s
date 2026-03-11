# Makefile for c-db-k8s
# make all
# make tests

# verbosity - aka Kbuild/HAProxy style
V = 0
Q = @
ifeq ($V,1)
Q=
endif

DEBUG ?= 0

ifeq ($(V),1)
cmd_TAR = $(TAR)
cmd_CC  = $(CC)
cmd_LD  = $(CC)
else
cmd_TAR  = $(Q)echo "  TAR   $@";$(TAR)
cmd_CC   = $(Q)echo "  CC    $@";$(CC)
cmd_LD   = $(Q)echo "  LD    $@";$(CC)
endif

# build commmand
INSTALL = install
TAR = tar
CC = gcc
LD = gcc

# compiler flags
CFLAGS += -D_GNU_SOURCE -Wall -Werror -O2 -Isrc -MMD -MP
ifeq ($(DEBUG), 1)
	CFLAGS += -O0 -g
endif
LDFLAGS = -static

# dirs
BUILD_DIR = build
SRC_DIR = src
BIN_DIR = bin
SCRIPTS_DIR = scripts

.PHONY: all
all: $(BUILD_DIR) cmds rootfs

# object files
$(BUILD_DIR):
	@mkdir -p $@

# where cmds are installed
$(BIN_DIR):
	@mkdir -p $@

# build our binaries
.PHONY: cmds
CMDS = server client launcher
BIN_CMDS = db/server client/client launcher
ROOTFS_CMDS = server client
CMDS_DONE = $(BUILD_DIR)/.cmds_done
$(CMDS_DONE): $(CMDS) | $(BUILD_DIR)
	@echo "  CMDS  done"
	@touch $@
cmds: $(CMDS_DONE)

# server
SERVER_SRCS = src/util.c src/sock.c src/db.c src/server.c
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
SERVER_DEPS = $(SERVER_OBJS:.o=.d)
-include $(SERVER_DEPS)
server: $(SERVER_OBJS)
	$(cmd_LD) $(LDFLAGS) $(SERVER_OBJS) -o $@

# client
CLIENT_SRCS = src/util.c src/client.c
CLIENT_OBJS = $(CLIENT_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CLIENT_DEPS = $(CLIENT_OBJS:.o=.d)
-include $(CLIENT_DEPS)
client: $(CLIENT_OBJS)
	$(cmd_LD) $(LDFLAGS) $(CLIENT_OBJS) -o $@

# launcher
LAUNCHER_LIBS = $(SECURITY_LIBS)
LAUNCHER_SRCS = src/util.c src/launcher.c
LAUNCHER_OBJS = $(LAUNCHER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
LAUNCHER_DEPS = $(LAUNCHER_OBJS:.o=.d)
-include $(LAUNCHER_DEPS)
launcher: $(LAUNCHER_OBJS)
	$(cmd_LD) $(LDFLAGS) $(LAUNCHER_OBJS) $(LAUNCHER_LIBS) -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(cmd_CC) $(CFLAGS) -c $< -o $@

# tests
.PHONY: test
test: test-server test-client

TEST_REQ_FILE = tests/test_req.txt
TEST_RSP_FILE = tests/test_rsp.txt
TEST_PORT := 6379
TEST_ADDR = 127.0.0.1
TEST_HOSTPORT = $(TEST_ADDR):$(TEST_PORT)
TEST_SERVER_LOG = $(BUILD_DIR)/test-server.log
TEST_WAIT_RUN = 0.5

RUN_TEST = echo "$$CMD" | nc -w 1 -N $(TEST_ADDR) $(TEST_PORT) | \
	grep -q "$$EXPECT" && \
    echo "TEST '$$CMD' PASSED" || \
    (echo "TEST '$$CMD' FAILED"; exit 1)

CHECK_ALIVE = \
	 if ! kill -0 $(1) 2>/dev/null; then \
		wait $(1); EXIT_CODE=$$?; \
        echo "FAIL: $(2) exit_code $$EXIT_CODE). Check $(3)"; \
		exit 1; \
   	fi

.PHONY: test-server
test-server: server
	$(Q)echo "[ TEST-START $@ ]"; \
	echo "Running server at $(TEST_HOSTPORT)"; \
	./server $(TEST_HOSTPORT) 1> $(TEST_SERVER_LOG) 2>&1 & SERVER_PID=$$!; \
	echo "Checking server at PID $$SERVER_PID"; \
	sleep $(TEST_WAIT_RUN); \
	$(call CHECK_ALIVE, $$SERVER_PID, "server", $(TEST_SERVER_LOG)); \
	echo "Checking server at $(TEST_HOSTPORT)"; \
	timeout 3 bash -c 'until nc -z $(TEST_ADDR) $(TEST_PORT); do sleep 0.1; done' || \
			(echo "FAIL: Server $$SERVER_PID at $(TEST_HOSTPORT) failed to connect!"; kill $$SERVER_PID 2>/dev/null; exit 1); \
	echo "Running tests..."; \
	CMD="SET foo bar";  EXPECT="OK"; $(RUN_TEST); \
	CMD="GET foo"; EXPECT="bar"; $(RUN_TEST); \
	CMD="DEL foo"; EXPECT="OK"; $(RUN_TEST); \
	CMD="GET foo"; EXPECT="FAIL"; $(RUN_TEST); \
	CMD="SET key value1"; EXPECT="OK"; $(RUN_TEST); \
	CMD="GET key"; EXPECT="value1"; $(RUN_TEST); \
	CMD="SET key value2"; EXPECT="OK"; $(RUN_TEST); \
	CMD="GET key"; EXPECT="value2"; $(RUN_TEST); \
	echo "Stopping server at PID $$SERVER_PID"; \
	kill $$SERVER_PID; \
	echo "[ TEST-DONE $@ ]"


BUILD_REQ_FILE = $(BUILD_DIR)/$(notdir $(TEST_REQ_FILE))
BUILD_RSP_FILE = $(BUILD_DIR)/$(notdir $(TEST_RSP_FILE))
SIMPLE_SERVER = scripts/simple_server.awk

.PHONY: test-client
test-client: client
	$(Q)echo "[ TEST-START $@ ]"; \
	rm -f $(BUILD_REQ_FILE) $(BUILD_RSP_FILE); \
	echo "Running $(SIMPLE_SERVER)"; \
	awk -f ./$(SIMPLE_SERVER) \
		-v Port="$(TEST_PORT)" \
		-v LogFile="$(BUILD_REQ_FILE)" \
		-v RespFile="$(TEST_RSP_FILE)" \
		& SERVER_PID=$$!; \
	echo "Checking $(SIMPLE_SERVER) at PID $$SERVER_PID"; \
	sleep $(TEST_WAIT_RUN); \
	$(call CHECK_ALIVE,$$SERVER_PID, $(SIMPLE_SERVER)); \
	echo "Sending $(TEST_REQ_FILE) via client" ; \
	cat $(TEST_REQ_FILE) | timeout 2s ./client localhost $(TEST_PORT) > $(BUILD_RSP_FILE); \
	sed -i -e 's/^> //' -e '/^\[+]/d' $(BUILD_RSP_FILE); \
	echo "Stopping $(SIMPLE_SERVER) at PID $$SERVER_PID"; \
	kill -9 $$SERVER_PID 2>/dev/null || true; \
	diff -q $(TEST_REQ_FILE) $(BUILD_REQ_FILE) && echo "TEST $(TEST_REQ_FILE) PASSED" || (echo "TEST $(TEST_REQ_FILE) FAILED"; exit 1); \
	diff -q $(TEST_RSP_FILE) $(BUILD_RSP_FILE) && echo "TEST $(TEST_RSP_FILE) PASSED" || (echo "TEST $(TEST_RSP_FILE) FAILED"; exit 1); \
	echo "[ TEST-DONE $@ ]"
		

# generate new security rules
.PHONY: gen_seccomp
CMD_FILE   = tests/test_req.txt
GEN_SECCOMP = $(SCRIPTS_DIR)/gen_seccomp.awk
STRACE_SRV = $(BUILD_DIR)/strace_srv.txt
STRACE_CLI = $(BUILD_DIR)/strace_cli.txt
STRACE_RAW = $(BUILD_DIR)/strace_raw.txt
SECCOMP_H  = $(BUILD_DIR)/seccomp_rules.h
gen_seccomp: client server
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


# 
# roofs
# ======
# build the container rootfs
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
	@LOADER_PATH=$$(readelf -l $(ROOTFS_DIR)/bin/bash | grep "program interpreter" | awk '{print $$NF}' | tr -d '[]') ; \
		install -D $$LOADER_PATH $(ROOTFS_DIR)/lib/$${LOADER_PATH}
	@echo "  + created $(ROOTFS_DIR)"
	@touch $@

# package rootfs
$(ROOTFS_TAR) : $(ROOTFS_DONE)
	$(cmd_TAR) -czf $@ $(ROOTFS_DIR)

# seccomp filter

# =======
# install
# =======
.PHONY: install
install : all
	@mkdir -p $(BIN_DIR)
	$(INSTALL) -D -m 755 server $(BIN_DIR)/db/server
	$(INSTALL) -D -m 755 client $(BIN_DIR)/client/client
	$(INSTALL) -D -m 755 launcher $(BIN_DIR)


.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(ROOTFS_DIR) $(CMDS) $(BIN_DIR) tags



# VM stuff

# alpine linux
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

.PHONY: show-vmconfig
show-vmconfig:
	@echo "MIRROR=$(MIRROR)"
	@echo "REL_URL=$(REL_URL)"
	@echo "REL_FILE=$(REL_FILE)"
	@echo "REL_VER=$(REL_VER)"
	@echo "CACHE_DIR=$(CACHE_DIR)"
	@echo "BASE_IMAGE=$(BASE_IMAGE)"
	@echo "RUN_IMAGE=$(RUN_IMAGE)"

.PHONY: list-vm
list-vm:
	virsh dominfo $(VM_NAME) || true
	virsh domifaddr $(VM_NAME) || true

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

# XXX alpline vms will use user-data.yaml to autoconfigure
USER_DATA = tests/user-data.yaml
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

wipe-vm:
	 virsh destroy $(VM_NAME)  || true
	 virsh undefine $(VM_NAME) || true

