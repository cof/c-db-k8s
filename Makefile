# Makefile for c-db-k8s
# make all
# make tests

# verbosity - aka Kbuild/HAProxy style
V = 0
Q = @
ifeq ($V,1)
Q=
endif

ifeq ($(V),1)
cmd_INST = $(INSTALL)
cmd_TAR = $(TAR)
cmd_CC  = $(CC)
cmd_LD  = $(CC)
else
cmd_INST = $(Q)echo "  INST  $@";$(INSTALL)
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
#CFLAGS = -Wall -Wextra -O2
CFLAGS += -D_GNU_SOURCE -Wall -O2 -Isrc -MMD -MP
ifdef DEBUG 
	CFLAGS += -O0 -g
endif
LDFLAGS = -static

ifeq ($(SECURITY),1)
	CFLAGS += -DSECURITY
	SECURITY_LIBS = -lcap -lseccomp
endif


# dirs
BUILD_DIR = build
SRC_DIR = src
DST_DIR = bin

.PHONY: all
all: $(BUILD_DIR) cmds rootfs install

$(BUILD_DIR):
	@mkdir -p $@

$(DST_DIR):
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
SERVER_SRCS = src/sock.c src/db.c src/server.c
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
SERVER_DEPS = $(SERVER_OBJS:.o=.d)
-include $(SERVER_DEPS)
server: $(SERVER_OBJS)
	$(cmd_LD) $(LDFLAGS) $(SERVER_OBJS) -o $@

# client
CLIENT_SRCS = src/client.c
CLIENT_OBJS = $(CLIENT_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CLIENT_DEPS = $(CLIENT_OBJS:.o=.d)
-include $(CLIENT_DEPS)
client: $(CLIENT_OBJS)
	$(cmd_LD) $(LDFLAGS) $(CLIENT_OBJS) -o $@

# launcher
LAUNCHER_LIBS = $(SECURITY_LIBS)
LAUNCHER_SRCS = src/launcher.c
LAUNCHER_OBJS = $(LAUNCHER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
LAUNCHER_DEPS = $(LAUNCHER_OBJS:.o=.d)
-include $(LAUNCHER_DEPS)
launcher: $(LAUNCHER_OBJS)
	$(cmd_LD) $(LDFLAGS) $(LAUNCHER_OBJS) $(LAUNCHER_LIBS) -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(cmd_CC) $(CFLAGS) -c $< -o $@

# build the container rootfs
.PHONY: rootfs
OUR_CMDS =
AUX_CMDS = bash ls ip ping hostname
ROOTFS_DIR = rootfs
ROOTFS_TAR = rootfs.tar.gz
ROOTFS_DONE = $(BUILD_DIR)/.rootfs_done
rootfs: $(ROOTFS_TAR)

# build rootfs
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

# install files
.PHONY: install
install: $(BIN_CMDS:%=$(DST_DIR)/%)
	@echo "  DONE  all files in $(DST_DIR)"

$(BIN_CMDS:%=$(DST_DIR)/%): $(DST_DIR)/% :
	@mkdir -p $(dir $@)
	$(cmd_INST) -m 755 $(notdir $*) $@

$(DST_DIR)/$(ROOTFS_TAR): $(ROOTFS_TAR) | $(DST_DIR)
	$(cmd_INST) -m 644 $< $@

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(ROOTFS_DIR) $(ROOTFS_TAR) $(CMDS) bin

.PHONY: test
test: server
	@echo "Starting tests"; \
	./server & SERVER_PID=$$!; \
    timeout 3 bash -c 'until nc -z localhost 6379; do sleep 0.1; done'; \
	echo "SET foo bar" | nc -w 1 -N localhost 6379 | grep -q "OK" || echo "SET failed"; \
	echo "GET foo" | nc -w 1 -N localhost 6379 | grep -q "bar" || echo "GET failed"; \
	echo "DEL foo" | nc -w 1 -N localhost 6379 | grep -q "OK" || echo "DEL failed"; \
	kill $$SERVER_PID


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
	virsh list --all
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

