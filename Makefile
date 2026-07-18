#
# Makefile for c-db-k8s
#
# Targets
# -------
#  all         : build cmds (client|server|launcher)
#  install     : put all cmds into bin folder
#  deploy      : builds images|create cluster|load images|deploy pods
#
#  test        : run basic tests (test-cmds)
#  test-full   : run all tests (test-cmds,test-lau,test-k8s)
#  test-cmds   : run cmds(client,server) tests
#  test-lau    : run launcher tests (using VM)
#  test-k8s    : run k8s tests (wait-pods,test-pod,test-net)
#  test-server : build and test ./server
#  test-client : build and test ./client
#  wait-pods   : wait for all pods to be ready
#  test-pod    : test SET|GET|DEL cmds on pods
#  test-net    : test k8s network policy
#
#  clean       : Remove compiled binaries, object files, and test logs
#  clean-k8s   : Remove cluster and docker images
#  spotless    : wipe everthing
# ------------------------------------
#
# Deps
# ====
# - gcc build tools
# - awk for tests
# - docker - images
# - k3d - cluster

# #######################
#     Config
# #######################
BUILD_DIR = build
SRC_DIR = src
BIN_DIR = bin
SCRIPTS_DIR = scripts
CMDS = server client launcher
MAKEFLAGS += --no-print-directory

# build tools
# -----------
INSTALL = install
TAR = tar
CC = gcc
LD = gcc
CTAGS = ctags
K3D = k3d

include scripts/verbose.mk


# compiler flags
# --------------
GCC_DEPS      := -MMD -MP
CPP_FLAGS     := -D_GNU_SOURCE -Isrc
EXTRA_CFLAGS  := -Wextra -Wno-missing-field-initializers
COMMON_CFLAGS := -Wall \
	-Wformat -Wformat-signedness \
	-Werror=sign-compare \
	-Werror=discarded-qualifiers \
	-Werror=shadow=compatible-local \
	-Werror=implicit-function-declaration \
	-Werror=missing-prototypes \
	$(CPP_FLAGS) $(GCC_DEPS)
DEBUG_CFLAGS  := -ggdb3 -fno-omit-frame-pointer -DDEBUG=1

# release build
# -----------
CFLAGS  = -O2 $(COMMON_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS = --static


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

# API
# ---
API_SRCS = src/util.c src/str_util.c src/log.c src/rwbuf.c src/dns_proto.c src/dns_resolv.c src/sock.c

# server
# ------
SERVER_SRCS = $(API_SRCS) src/db.c src/server.c
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
SERVER_DEPS = $(SERVER_OBJS:.o=.d)
-include $(SERVER_DEPS)
server: $(SERVER_OBJS)
	$(cmd_LD) $(CFLAGS) $(LDFLAGS) $(SERVER_OBJS) -o $@

# client
# ------
CLIENT_SRCS = $(API_SRCS) src/client.c
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
	@strace -c -o $(STRACE_SRV) ./server $(CMD_ARGS) & STRACE_PID=$$!; \
	SERV_PID=$$(pgrep -P $$STRACE_PID); \
	echo "Profiling Server (PID: $$SERV_PID) via Strace (PID: $$STRACE_PID)"; \
	sleep 1; \
	strace -c -o $(STRACE_CLI) ./client $(CMD_ARGS) < $(CMD_FILE) 1>/dev/null || true; \
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
VM_NAME = test-lau
include scripts/build_vm.mk
build-vm: vm-create

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

# ###########
# TEST SUITES
# ###########:
include scripts/tests.mk

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
	./server $(CMD_ARGS) 1> $(SRV_LOGFILE) 2>&1 & SRV_PID=$$!; \
	$(call WAIT_PIDUP,$$SRV_PID,server); \
	$(call WAIT_CONNUP,$(TEST_ADDR),$(TEST_PORT),1, $$SRV_PID); \
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
	$(call WAIT_CONNUP,$(TEST_ADDR),$(TEST_PORT),1, $$SRV_PID); \
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
test-lau: $(INSTALL_DONE) build-vm
	$(Q)echo "[+] Running $@"; \
	VM_IP=$$($(VM_GET_IP)); \
	if [ -z "$$VM_IP" ]; then echo "[ERROR] No VM ip address"; exit 1; fi; \
	VM_SSH_ADDR="$(VM_USER)@$$VM_IP"; \
	echo " => Copying $(BIN_DIR) to $$VM_SSH_ADDR:$(VM_HOME)"; \
	scp -q $(VM_SSH_OPTS) -r $(BIN_DIR) $$VM_SSH_ADDR:$(VM_HOME); \
	echo " => Sending cmds to $(VM_NAME) ..."; \
	$(call SEND_CMDS,$(TEST_REQFILE)) | ssh -tt $(VM_SSH_OPTS) $$VM_SSH_ADDR "$(LAU_CMD)" 2>&1 | tr -d '\r' | tee $(LAU_LOGFILE); \
	echo " => Fetching logs"; \
	sed -n 's/^> //; /GET \|SET \|DEL \|QUIT/p' $(LAU_LOGFILE) >$(RES_REQFILE); \
	sed -n '/^[A-Za-z0-9]/p' $(LAU_LOGFILE) >$(RES_RSPFILE); \
	total=0; errors=0; \
	$(call DIFF_FILE,$(TEST_REQFILE),$(RES_REQFILE),$(LAU_LOGFILE)); \
	$(call DIFF_FILE,$(TEST_RSPFILE),$(RES_RSPFILE),$(LAU_LOGFILE)); \
	$(call TEST_REPORT); \
	if [ $$errors -gt 0 ]; then exit 1; fi
	@echo "$(CHECK) $@ complete."

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
	@echo "Removing build-dirs,cmds,tags"
	$(Q)rm -rf $(BUILD_DIR) $(BIN_DIR) $(CMDS) tags

.PHONY: clean-all
clean-all: clean-k8s clean-rootfs vm-clean clean
	@echo "Clean done"

.PHONY: spotless
spotless: clean-all
	@echo "Wiping everthing"
	-docker system prune -af --volumes

