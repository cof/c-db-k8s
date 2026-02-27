# Makefile for c-db-k8s
# make all
# make tests

CC = gcc
#CFLAGS = -Wall -Wextra -O2
CFLAGS += -D_GNU_SOURCE -Wall -O2 -Isrc -MMD -MP
ifdef DEBUG 
	CFLAGS += -O0 -g
endif
LDFLAGS = -static


BUILD_DIR = build
SRC_DIR = src

.PHONY: all
all: $(BUILD_DIR) cmds publish

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

.PHONY: cmds
CMDS = server client launcher
cmds : $(CMDS)

# server
SERVER_SRCS = src/util.c src/sock.c src/db.c src/server.c
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
SERVER_DEPS = $(SERVER_OBJS:.o=.d)
-include $(SERVER_DEPS)
server: $(SERVER_OBJS)
	$(E) "  LD       $@"
	$(Q)$(CC) $(SERVER_OBJS) -o $@

# client
CLIENT_SRCS = src/client.c
CLIENT_OBJS = $(CLIENT_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CLIENT_DEPS = $(CLIENT_OBJS:.o=.d)
-include $(CLIENT_DEPS)
client: $(CLIENT_OBJS)
	$(E) "  LD       $@"
	$(Q)$(CC) $(CLIENT_OBJS) -o $@

# launcher
LAUNCHER_SRCS = src/launcher.c
LAUNCHER_OBJS = $(LAUNCHER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
LAUNCHER_DEPS = $(LAUNCHER_OBJS:.o=.d)
-include $(LAUNCHER_DEPS)
launcher: $(LAUNCHER_OBJS)
	$(E) "  LD       $@"
	$(Q)$(CC) $(LAUNCHER_OBJS) -o $@

# XXX force BUILD_DIR as a prerequisite
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(E) "  CC       $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@


.PHONY: publish
publish : bincmds rootfs

# put CMDS into bin
.PHONY: bincmds
BINCMDS_DONE = $(BUILD_DIR)/.bincmds_done
bincmds: cmds $(BINCMDS_DONE)
$(BINCMDS_DONE) : $(CMDS) | $(BUILD_DIR)
	@mkdir -p bin
	@for cmd in $^; do \
		echo "INSTALL -> $$cmd bin/$$cmd"; \
		install -D $$cmd bin/; \
	done
	@touch $@

# build rootfs
.PHONY: rootfs
OUR_CMDS = server client
AUX_CMDS = bash ls ip ping hostname
STAGING_DIR = build_rootfs
ROOTFS_DONE = $(BUILD_DIR)/.rootfs_done
rootfs: bin/assets/rootfs.tgz

# package rootfs into a tarball
bin/assets/rootfs.tgz : $(ROOTFS_DONE)
	@mkdir -p bin/assets
	@tar -czf $@ $(STAGING_DIR)
	@echo "TAR - $@"

# build rootfs
$(ROOTFS_DONE): $(OUR_CMDS) | $(BUILD_DIR)
	@echo "Build rootfs"
	@mkdir -p $(STAGING_DIR)  $(STAGING_DIR)/bin $(STAGING_DIR)/lib
	@echo "+ binaries - $(OUR_CMDS)"
	@for cmd in $^; do \
		install -D $$cmd $(STAGING_DIR)/bin; \
	done
	@echo "+ helpers - $(AUX_CMDS)"
	@for cmd in $(AUX_CMDS); do \
		CMD_PATH=$$(command -v $$cmd) || { echo "Error: $$cmd not found"; exit 1; }; \
	    install -D $$CMD_PATH $(STAGING_DIR)/bin/; \
	done 
	@echo "+ Shared libraires"
	@ldd $(STAGING_DIR)/bin/* | grep "=> /" | awk '{print $$3}' | sort -u | \
			xargs -I '{}' install -D '{}' $(STAGING_DIR)/lib/
	@echo "+ Dynamic Linker"
	@LOADER_PATH=$$(readelf -l $(STAGING_DIR)/bin/bash | grep "program interpreter" | awk '{print $$NF}' | tr -d '[]') ; \
		install -D $$LOADER_PATH $(STAGING_DIR)/lib/$${LOADER_PATH}
	@touch $@


.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(STAGING_DIR) $(CMDS) bin

.PHONY: test
test: server
	@echo "Starting tests"; \
	./server & SERVER_PID=$$!; \
    timeout 3 bash -c 'until nc -z localhost 6379; do sleep 0.1; done'; \
	echo "SET foo bar" | nc -w 1 -N localhost 6379 | grep -q "OK" || echo "SET failed"; \
	echo "GET foo" | nc -w 1 -N localhost 6379 | grep -q "bar" || echo "GET failed"; \
	echo "DEL foo" | nc -w 1 -N localhost 6379 | grep -q "OK" || echo "DEL failed"; \
	kill $$SERVER_PID


# nocloud alpine
USER_DATA = tests/user-data.yaml
OS_VARIANT= alpinelinux3.21
OS_NAME=alpine
REL_VER= 3.21
PATCH_VER=.6
IMAGE_VER = $(OS_NAME)-$(REL_VER)$(PATCH_VER)
IMAGE_NAME = nocloud_$(IMAGE_VER)-x86_64-bios-cloudinit-r0.qcow2
REL_DIR=v$(REL_VER)
MIRROR_URL = https://dl-cdn.alpinelinux.org
IMAGE_URL = $(MIRROR_URL)/$(REL_DIR)/releases/cloud/$(IMAGE_NAME)
IMAGE_PATH = $(BUILD_DIR)/$(IMAGE_NAME)
TEST_IMAGE = $(BUILD_DIR)/myalpine.qcow2
TEST_VM = test-launcher

# download image
$(IMAGE_PATH):
	wget -nv --no-verbose --show-progress -O $(IMAGE_PATH) $(IMAGE_URL)

# build image
# XXX  virt-customize requires root read /boot/vmlinux.
$(TEST_IMAGE) : $(IMAGE_PATH)
	@echo "Setting up image file"
	@cp $(IMAGE_PATH) $(TEST_IMAGE)
	virt-customize -a $(TEST_IMAGE) \
	--root-password password:test \
	--install openssh \
	--edit '/etc/ssh/sshd_config: s/\#PermitRootLogin.*/PermitRootLogin yes/' \
	--run-command "rc-update add sshd"
	@touch $(TEST_IMAGE)

# install image
install-vms: $(IMAGE_PATH)
	virt-install \
	--name $(TEST_VM) \
	--virt-type kvm \
	--ram 512 \
	--vcpus 1 \
	--disk path=$(IMAGE_PATH),format=qcow2,bus=virtio \
	--network network=default,model=virtio \
	--cloud-init user-data=$(USER_DATA) \
	--os-variant $(OS_VARIANT) \
	--graphics vnc \
	--rng /dev/urandom \
	--noautoconsole \
	--import

wipe-vms:
	 virsh destroy $(TEST_VM)  || true
	 virsh undefine $(TEST_VM) || true

ifneq ($(V),1)
  Q = @
  E = @echo
else
  Q =
  E = @true
endif
