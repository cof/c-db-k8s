# #################
# TEST SUITE MACROS 
# #################

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
	kill -0 $(1) 2>/dev/null; \
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
SRV_LOGFILE = $(BUILD_DIR)/test_server.log
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
