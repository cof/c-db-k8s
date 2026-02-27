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
TARGETS = server client launcher

.PHONY: all clean test

all: $(BUILD_DIR) $(TARGETS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# server
SERVER_SRCS = src/util.c src/sock.c src/db.c src/server.c
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
SERVER_DEPS = $(SERVER_OBJS:.o=.d)
-include $(SERVER_DEPS)
server: $(SERVER_OBJS)
	$(CC) $(SERVER_OBJS) -o $@

# client
CLIENT_SRCS = src/client.c
CLIENT_OBJS = $(CLIENT_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CLIENT_DEPS = $(CLIENT_OBJS:.o=.d)
-include $(CLIENT_DEPS)
client: $(CLIENT_OBJS)
	$(CC) $(CLIENT_OBJS) -o $@

# launcher
LAUNCHER_SRCS = src/launcher.c
LAUNCHER_OBJS = $(LAUNCHER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
LAUNCHER_DEPS = $(LAUNCHER_OBJS:.o=.d)
-include $(LAUNCHER_DEPS)
launcher: $(LAUNCHER_OBJS)
	$(CC) $(LAUNCHER_OBJS) -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGETS)

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
	

# XXX force BUILD_DIR as a prerequisite
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
