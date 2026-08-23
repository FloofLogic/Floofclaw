CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -D_DARWIN_C_SOURCE -D_GNU_SOURCE -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
BUILD_REVISION ?= $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

# The base flags before this file adds anything. Targets that re-invoke make
# with their own CFLAGS (asan, ubsan) start from this, so the sub-make runs
# its own library detection once instead of inheriting an already-augmented
# set and appending a second copy of everything.
FCLAW_BASE_CFLAGS := $(CFLAGS)

# Everything below is appended with `override`. A plain `+=` after a
# command-line `CFLAGS=...` is silently discarded by make, which would drop
# the feature defines and include paths the sources need and produce a
# binary that compiles but cannot reach a provider. `override` lets
# `make CFLAGS=-O0` refine the base flags without disarming the build.
ifeq ($(findstring -DFCLAW_BUILD_REVISION,$(CFLAGS)),)
override CFLAGS += -DFCLAW_BUILD_REVISION=\"$(BUILD_REVISION)\"
endif

# MOCK_ONLY=1 builds the advertised no-optional-library runtime: no libcurl
# HTTP transport, no OpenSSL TLS. It is the supported way to get that build,
# and the only way to get it without an explicit request.
ifeq ($(strip $(MOCK_ONLY)),1)
  # Skipping detection is not enough. A caller-supplied CFLAGS/LDFLAGS can
  # already carry the feature defines and libraries — `make asan` re-passes
  # the parent's fully expanded flags to its sub-make, and MAKEFLAGS carries
  # them into any nested `make MOCK_ONLY=1`. Left alone, MOCK_ONLY would
  # define macros for transports it links nothing for, or link the very
  # libraries it claims to omit. Strip them.
  override CFLAGS  := $(filter-out -DFCLAW_HAVE_LIBCURL -DFCLAW_HAVE_OPENSSL,$(CFLAGS))
  override LDFLAGS := $(filter-out -lcurl -lssl -lcrypto,$(LDFLAGS))
  OPENSSL_PREFIX :=
  OPENSSL_CFLAGS :=
  OPENSSL_LIBS   :=
  CURL_CFLAGS    :=
  CURL_LIBS      :=
endif

# Optional OpenSSL — enables nonblocking TLS in runtime/support/net_tls.c.
# Channel adapters configured with tls: true (Discord, IRC-with-tls)
# require this. Without OpenSSL the TLS wrapper compiles to stubs that return
# FC_TLS_NOT_BUILT, the WS adapter refuses to start, and tls: true on any
# IRC config refuses to start with a clear error.
# Auto-detected: portable pkg-config first, then Homebrew's keg-only
# openssl@3. Override either with OPENSSL_PREFIX=/path.
ifneq ($(strip $(MOCK_ONLY)),1)
ifeq ($(strip $(OPENSSL_PREFIX)),)
  ifeq ($(shell pkg-config --exists openssl 2>/dev/null && echo yes),yes)
    OPENSSL_CFLAGS := -DFCLAW_HAVE_OPENSSL $(shell pkg-config --cflags openssl)
    OPENSSL_LIBS   := $(shell pkg-config --libs openssl)
  else
    OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null)
  endif
endif
ifneq ($(strip $(OPENSSL_PREFIX)),)
  OPENSSL_CFLAGS := -DFCLAW_HAVE_OPENSSL -I$(OPENSSL_PREFIX)/include
  OPENSSL_LIBS   := -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
endif
ifeq ($(strip $(OPENSSL_CFLAGS)),)
  $(warning OpenSSL not found via pkg-config or 'brew --prefix openssl@3'. Building without TLS.)
  $(warning   Channels requiring TLS (Discord WSS, IRC tls:true, WS adapter) will refuse to start.)
  $(warning   To enable: install openssl-devel/libssl-dev (Linux), brew install openssl@3 (macOS), or pass OPENSSL_PREFIX=/path.)
endif
endif
override CFLAGS  += $(OPENSSL_CFLAGS)
override LDFLAGS += $(OPENSSL_LIBS)

# Optional libcurl — enables real HTTP LLM transport (runtime/llm/call.c).
# Without it, the LLM call path returns -1 for every current non-mock provider
# kind, including loopback OpenAI-compatible local endpoints. The mock-backed
# test suite stays green
# without it, so this miss is easy to make and only surfaces at runtime.
ifneq ($(strip $(MOCK_ONLY)),1)
CURL_CFLAGS ?= $(shell curl-config --cflags 2>/dev/null || pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS   ?= $(shell curl-config --libs 2>/dev/null || pkg-config --libs libcurl 2>/dev/null)
endif
ifneq ($(strip $(CURL_LIBS)),)
  override CFLAGS  += -DFCLAW_HAVE_LIBCURL $(CURL_CFLAGS)
  override LDFLAGS += $(CURL_LIBS)
else ifeq ($(strip $(MOCK_ONLY)),1)
  $(info FloofClaw: MOCK_ONLY=1 — building without libcurl or OpenSSL.)
  $(info   Mock providers and local execution work; every network provider and TLS channel refuses to start.)
else
  $(error libcurl not detected via 'curl-config' or pkg-config. \
Every non-mock provider, including loopback OpenAI-compatible endpoints, would \
fail at the agent layer, so this build is refused rather than shipped silently. \
To enable: install libcurl-devel (Fedora) / libcurl4-openssl-dev (Debian) / \
brew install curl (macOS). If your headers are elsewhere, pass \
CURL_CFLAGS='-I.../include' CURL_LIBS='-L.../lib -lcurl'. To build the \
mock-only runtime deliberately, run: make MOCK_ONLY=1)
endif

SUPPORT_SRC = \
	runtime/fjson_repair/repair.c \
	runtime/support/fsutil.c \
	runtime/support/json.c \
	runtime/support/color.c \
	runtime/support/cli_output.c \
	runtime/support/timing.c \
	runtime/support/logo.c \
	runtime/support/net.c \
	runtime/support/net_tls.c \
	runtime/support/duration.c \
	runtime/support/heap_guard.c \
	runtime/support/scratch_guard.c \
	runtime/support/reconnect_backoff.c \
	runtime/support/log_rotation.c \
	runtime/support/sha256.c

SECURE_SRC = \
	runtime/secure/secret_store.c \
	runtime/secure/action_secret_broker.c \
	runtime/secure/auth_targets.c \
	runtime/secure/auth.c

LLM_SRC = \
	runtime/llm/profile.c \
	runtime/llm/mock.c \
	runtime/llm/usage.c \
	runtime/llm/normalize.c \
	runtime/llm/call.c \
	runtime/llm/provider_limit.c \
	runtime/llm/http.c \
	runtime/llm/stream.c \
	runtime/llm/request.c \
	runtime/llm/media.c

BUS_SRC = \
	runtime/bus/bus.c \
	runtime/bus/media_manifest.c

GATEWAY_SRC = \
	runtime/gateway/gateway.c \
	runtime/gateway/reactor.c \
	runtime/gateway/jobrunner.c \
	runtime/gateway/jobrunner_module.c \
	runtime/gateway/wake_module.c \
	runtime/gateway/bus_intake_module.c \
	runtime/gateway/runtime_module.c \
	runtime/gateway/status_module.c \
	runtime/gateway/affair_watcher_module.c \
	runtime/gateway/janitor_module.c

# runtime/channel/ holds the shared channel plumbing every adapter uses
# (channel_synchronous, the CLI client path). That is engine, not adapter.
CHANNEL_SRC = \
	runtime/channel/channel.c

# Channel adapters are self-contained source directories under adapters/.
# Adding one is: create adapters/<name>/, export a const FcAdapter
# fc_adapter_<name>, build. Nothing here is hand-listed — the wildcard finds
# the sources and ADAPTER_REGISTRY generates the extern declarations and the
# fc_adapters[] array from the directory names, sorted for a deterministic
# build. A directory that does not export its descriptor fails the link
# naming the missing symbol, which is the contract enforcing itself.
ADAPTER_SRC := $(sort $(wildcard adapters/*/*.c))
ADAPTER_NAMES := $(sort $(notdir $(patsubst %/,%,$(dir $(ADAPTER_SRC)))))
ADAPTER_REGISTRY := build/adapter_registry.gen.c

RUNTIME_SRC = \
	runtime/main.c \
	runtime/scheduler.c \
	runtime/recovery.c \
	runtime/run.c \
	runtime/run_state.c \
	runtime/action_output.c \
	runtime/publication_outbox.c \
	runtime/ports.c \
	runtime/agent_errors.c \
	runtime/agent_meta.c \
	runtime/agent_runner.c \
	runtime/action_runner.c \
	runtime/action_coerce.c \
	runtime/action_events.c \
	runtime/action_runtime.c \
	runtime/action_runtime_affair.c \
	runtime/action_runtime_files.c \
	runtime/action_runtime_work.c \
	runtime/action_runtime_workers.c \
	runtime/action_subprocess.c \
	runtime/internal_cli.c \
	runtime/floop.c \
	runtime/profile.c \
	runtime/agent_exec.c \
	runtime/agent_model.c \
	runtime/agent_selected.c \
	runtime/action_registry.c \
	runtime/event_log.c \
	runtime/state.c \
	runtime/memory.c \
	runtime/affair_state.c \
	runtime/task_state.c \
	runtime/work_state.c \
	runtime/operation_state.c \
	runtime/operation_driver.c \
	runtime/operation_cli.c \
	runtime/task_artifacts.c \
	runtime/task_json.c \
	runtime/task_archive.c \
	runtime/task_projection.c \
	runtime/agents.c \
	runtime/native_agents/noop.c \
	runtime/native_agents/memory.c \
	runtime/native_agents/workers.c \
	runtime/trace.c \
	runtime/view.c \
	runtime/pulse_projection.c \
	runtime/usage_cli.c \
	runtime/cacheview.c \
	runtime/inspection_stream.c \
	runtime/action_cli.c \
	runtime/config_cli.c \
	runtime/qol_cli.c \
	runtime/auth_cli.c \
	runtime/bus_cli.c \
	runtime/affair_cli.c \
	runtime/gateway_cli.c \
	runtime/channel_cli.c \
	runtime/json.c \
	$(SUPPORT_SRC) \
	$(SECURE_SRC) \
	$(LLM_SRC) \
	$(BUS_SRC) \
	$(GATEWAY_SRC) \
	$(CHANNEL_SRC) \
	$(ADAPTER_SRC) \
	$(ADAPTER_REGISTRY)

RUNTIME_HDRS = \
	runtime/fjson_repair/repair.h \
	runtime/runtime.h \
	runtime/run_state.h \
	runtime/task_internal.h \
	runtime/action_runtime_internal.h \
	runtime/inspection_stream.h \
	runtime/publication_outbox.h \
	runtime/floop.h \
	runtime/error_codes.h \
	runtime/version.h \
	runtime/llm/stream.h \
	runtime/agent_errors.h \
	runtime/agent_exec.h \
	runtime/agents.h \
	runtime/support/fsutil.h \
	runtime/support/json.h \
	runtime/support/stringhelp.h \
	runtime/support/color.h \
	runtime/support/cli_output.h \
	runtime/support/timing.h \
	runtime/support/logo.h \
	runtime/support/net.h \
	runtime/support/net_tls.h \
	runtime/support/duration.h \
	runtime/support/log_rotation.h \
	runtime/support/sha256.h \
	runtime/support/scratch_guard.h \
	runtime/support/reconnect_backoff.h \
	runtime/secure/secret_store.h \
	runtime/secure/action_secret_broker.h \
	runtime/secure/auth_targets.h \
	runtime/secure/auth.h \
	runtime/llm/llm.h \
	runtime/llm/request.h \
	runtime/llm/media.h \
	runtime/llm/provider_limit.h \
	runtime/bus/bus.h \
	runtime/bus/media_manifest.h \
	runtime/gateway/gateway.h \
	runtime/gateway/reactor.h \
	runtime/gateway/wake_module.h \
	runtime/gateway/jobrunner.h \
	runtime/gateway/jobrunner_module.h \
	runtime/gateway/bus_intake_module.h \
	runtime/gateway/runtime_module.h \
	runtime/gateway/status_module.h \
	runtime/gateway/janitor_module.h \
	runtime/channel/channel.h \
	runtime/gateway/adapter.h

# Unit tests — pure C, no fork. Call runtime library functions or scan
# source files directly. Should run in tens of milliseconds.
UNIT_TEST_SRC = \
	tests/test_runner.c \
	tests/test_support.c \
	tests/unit/test_budget.c \
	tests/unit/test_duration.c \
	tests/unit/test_reconnect_backoff.c \
	tests/unit/test_tls.c \
	tests/unit/test_fsutil.c \
	tests/unit/test_fjson_repair.c \
	tests/unit/test_json_escape.c \
	tests/unit/test_llm_request.c \
	tests/unit/test_llm_stream.c \
	tests/unit/test_provider.c

# Integration tests — in-process harness coverage for runtime budgets.
INTEGRATION_TEST_SRC = \
	tests/test_runner.c \
	tests/test_support.c \
	tests/integration/test_budget.c \
	tests/integration/test_gateway_hardening.c \
	tests/integration/test_media_ingress.c \
	tests/integration/test_typed_ingress.c \
	tests/integration/test_janitor_retention.c \
	tests/integration/test_adapter_tailers.c \
	tests/integration/test_reactor_pollset.c \
	tests/integration/test_channel_contexts.c \
	tests/integration/test_work_ledger_rebuild.c \
	tests/integration/test_media_handoff.c \
	tests/integration/test_llm_media_download.c \
	tests/integration/test_provider_limits.c \
	tests/integration/test_action_catalog.c \
	tests/integration/test_work_controller.c \
	tests/integration/test_run_fail_cleanup.c \
	tests/integration/test_floofclaw.c \
	tests/harness.c \
	tests/integration_support.c

# Runtime objects linked into bin/fclaw_integration_tests so the harness
# can call rt_scheduler_*, channel_synchronous, rt_view_main etc.
# directly instead of forking ./bin/fclaw. Same set as RUNTIME_SRC
# minus runtime/main.c (which has its own main() entry point).
INTEGRATION_RUNTIME_SRC = $(filter-out runtime/main.c,$(RUNTIME_SRC))

all: build

build: bin/fclaw

.PHONY: android-arm64 _print-runtime-src FORCE

# Cross-compile the ordinary FloofClaw CLI for Android/ARM64. The output is
# named `fclaw`; APK-specific renaming and packaging belong to the consuming
# Android application, not this repository.
android-arm64:
	./scripts/build_android_arm64.sh

# Depends on the generated registry: consumers such as the Android build
# check that every RUNTIME_SRC entry exists on disk, so it must be generated
# before the list is printed.
_print-runtime-src: $(ADAPTER_REGISTRY)
	@printf '%s\n' $(RUNTIME_SRC)

# FORCE, not a source list: a rule whose prerequisites are the files that
# still exist cannot notice a directory being *removed*. The recipe is a few
# printfs, so it runs every build and swaps the result in only when the
# adapter set actually changed — which keeps the relink rare while making
# add, remove, and rename all correct.
$(ADAPTER_REGISTRY): FORCE
	@mkdir -p build
	@{ \
		printf '/* Generated from the adapters/ directory listing. Do not edit. */\n'; \
		printf '#include "../runtime/gateway/adapter.h"\n\n'; \
		for name in $(ADAPTER_NAMES); do \
			printf 'extern const FcAdapter fc_adapter_%s;\n' "$$name"; \
		done; \
		printf '\nconst FcAdapter *const fc_adapters[] = {\n'; \
		for name in $(ADAPTER_NAMES); do \
			printf '  &fc_adapter_%s,\n' "$$name"; \
		done; \
		test -n "$(ADAPTER_NAMES)" || printf '  (const FcAdapter *)0,\n'; \
		printf '};\nconst size_t fc_adapter_count = %d;\n' $(words $(ADAPTER_NAMES)); \
	} > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

FORCE:

bin/fclaw: $(RUNTIME_SRC) $(RUNTIME_HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(LDFLAGS) $(RUNTIME_SRC) -o $@

bin/fclaw_unit_tests: $(UNIT_TEST_SRC) runtime/fjson_repair/repair.c runtime/support/json.c runtime/support/fsutil.c runtime/support/duration.c runtime/support/heap_guard.c runtime/support/reconnect_backoff.c runtime/support/net_tls.c runtime/llm/normalize.c runtime/llm/profile.c runtime/llm/request.c runtime/llm/stream.c runtime/llm/provider_limit.c runtime/llm/usage.c runtime/secure/auth.c runtime/secure/auth_targets.c runtime/secure/secret_store.c runtime/secure/action_secret_broker.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(LDFLAGS) -Itests \
		-DTEST_REGISTRY_INC='"test_registry_unit.inc"' \
		-DDEFAULT_BUDGET_MS=100 \
		$(UNIT_TEST_SRC) runtime/fjson_repair/repair.c runtime/support/json.c runtime/support/fsutil.c runtime/support/duration.c runtime/support/heap_guard.c runtime/support/reconnect_backoff.c runtime/support/net_tls.c runtime/llm/normalize.c runtime/llm/profile.c runtime/llm/request.c runtime/llm/stream.c runtime/llm/provider_limit.c runtime/llm/usage.c runtime/secure/auth.c runtime/secure/auth_targets.c runtime/secure/secret_store.c runtime/secure/action_secret_broker.c runtime/action_coerce.c -o $@

bin/fclaw_integration_tests: $(INTEGRATION_TEST_SRC) $(INTEGRATION_RUNTIME_SRC) $(RUNTIME_HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(LDFLAGS) -Itests \
		-DTEST_REGISTRY_INC='"test_registry_integration.inc"' \
		-DDEFAULT_BUDGET_MS=300 \
		$(INTEGRATION_TEST_SRC) $(INTEGRATION_RUNTIME_SRC) -o $@

bin/fclaw_chaos_check: tests/chaos/check.c runtime/state.c runtime/support/json.c runtime/support/fsutil.c runtime/support/heap_guard.c runtime/runtime.h runtime/support/json.h runtime/support/fsutil.h runtime/support/heap_guard.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(LDFLAGS) tests/chaos/check.c runtime/state.c \
		runtime/support/json.c runtime/support/fsutil.c runtime/support/heap_guard.c -o $@

bin/fclaw_thrash_driver: tests/thrash/driver.c runtime/bus/bus.c runtime/support/json.c runtime/support/fsutil.c runtime/support/heap_guard.c runtime/runtime.h runtime/runtime_kernel.h runtime/bus/bus.h runtime/support/json.h runtime/support/fsutil.h runtime/support/heap_guard.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(LDFLAGS) tests/thrash/driver.c runtime/bus/bus.c \
		runtime/support/json.c runtime/support/fsutil.c runtime/support/heap_guard.c -o $@

bin/fclaw_local_client_probe: tests/local_client_probe.c
	@mkdir -p bin
	$(CC) $(CFLAGS) tests/local_client_probe.c -o $@

unit: bin/fclaw_unit_tests
	./tests/run_in_isolated_copy.sh ./bin/fclaw_unit_tests

integration: build bin/fclaw_integration_tests
	./tests/run_in_isolated_copy.sh ./bin/fclaw_integration_tests
	./tests/test_fixture_isolation.sh

test: unit integration bin/fclaw_local_client_probe
	./tests/run_in_isolated_copy.sh bash ./tests/test_cli_output_modes.sh
	./tests/run_in_isolated_copy.sh bash ./tests/test_version_contract.sh
	./tests/run_in_isolated_copy.sh ./tests/test_local_client_api.sh
	./tests/run_in_isolated_copy.sh bash ./tests/test_action_cli.sh
	./tests/run_in_isolated_copy.sh bash ./tests/test_config_cli.sh
	./tests/run_in_isolated_copy.sh bash ./tests/test_bus_typed_ingress.sh
	./tests/run_in_isolated_copy.sh bash ./tests/test_mock_only_build_contract.sh
	./tests/run_in_isolated_copy.sh bash ./tests/test_web_read_action.sh
	./tests/run_in_isolated_copy.sh bash ./tests/test_tokenwatch_app.sh
	./tests/run_in_isolated_copy.sh bash ./tests/test_managed_handle_restart.sh
	./tests/run_in_isolated_copy.sh bash ./tests/test_manage_hermes_completion.sh
	./tests/test_android_arm64_build_contract.sh
	./tests/smoke_lookup_and_poem.sh

test-small-stack:
	@bash -c 'ulimit -s 1024; exec $(MAKE) test'

# Built from the base flags, not the augmented ones: the sub-make appends
# its own detected defines and libraries with `override`, so re-passing them
# here would define FCLAW_BUILD_REVISION twice with two different values.
SANITIZER_CFLAGS = $(filter-out -O%,$(FCLAW_BASE_CFLAGS)) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
SANITIZER_LDFLAGS = -fsanitize=address,undefined

# libFuzzer needs a clang that ships the fuzzer runtime. Apple clang
# does NOT; install real LLVM (`brew install llvm`) and build with
# FUZZ_CC=/opt/homebrew/opt/llvm/bin/clang (the default here). The
# fjson_repair bugs found by this harness are pinned as fast unit
# regressions in tests/unit/test_fjson_repair.c, which run under the
# normal ASan/UBSan gates even on machines without libFuzzer.
FUZZ_CC ?= /opt/homebrew/opt/llvm/bin/clang
FUZZ_CFLAGS = $(filter-out -O%,$(CFLAGS)) -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined
FUZZ_LDFLAGS = $(LDFLAGS) -fsanitize=fuzzer,address,undefined
# heap_guard.c provides fc_xmalloc/fc_xfree used by json.c; fuzz_stubs.c
# provides rt_exe_path() (normally in main.c, which the fuzz targets
# do not link) for the full-runtime event_reducer target.
FUZZ_BINS = bin/fuzz_json bin/fuzz_fjson_repair bin/fuzz_event_reducer

bin/fuzz_json: tests/fuzz/fuzz_json.c runtime/support/json.c runtime/support/heap_guard.c runtime/support/json.h
	@mkdir -p bin
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_json.c runtime/support/json.c runtime/support/heap_guard.c $(FUZZ_LDFLAGS) -o $@

bin/fuzz_fjson_repair: tests/fuzz/fuzz_fjson_repair.c runtime/fjson_repair/repair.c runtime/fjson_repair/repair.h runtime/support/json.c runtime/support/heap_guard.c runtime/support/json.h
	@mkdir -p bin
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_fjson_repair.c runtime/fjson_repair/repair.c runtime/support/json.c runtime/support/heap_guard.c $(FUZZ_LDFLAGS) -o $@

bin/fuzz_event_reducer: tests/fuzz/fuzz_event_reducer.c tests/fuzz/fuzz_stubs.c $(INTEGRATION_RUNTIME_SRC) $(RUNTIME_HDRS)
	@mkdir -p bin
	$(FUZZ_CC) $(FUZZ_CFLAGS) tests/fuzz/fuzz_event_reducer.c tests/fuzz/fuzz_stubs.c $(INTEGRATION_RUNTIME_SRC) $(FUZZ_LDFLAGS) -o $@

fuzz-build: $(FUZZ_BINS)

fuzz: fuzz-build
	./tests/fuzz/run.sh

CHAOS_ITERATIONS ?= 500
CHAOS_SEED ?= 424242
CHAOS_MODE ?= crash
CHAOS_RAMDISK_MIB ?= 16
THRASH_DURATION_SECONDS ?= 600
THRASH_RATE ?= 60
THRASH_MIN_RATE ?= 50
THRASH_CONTEXTS ?= 64
THRASH_PREFILL ?= 32
THRASH_DRAIN_TIMEOUT_SECONDS ?= 300
THRASH_RSS_KB_MAX ?= 1048576
THRASH_FOOTPRINT_KB_MAX ?= 262144
THRASH_ASAN_FOOTPRINT_KB_MAX ?= 524288
THRASH_EFFECTIVE_FOOTPRINT_KB_MAX = $(if $(findstring fsanitize=address,$(CFLAGS)),$(THRASH_ASAN_FOOTPRINT_KB_MAX),$(THRASH_FOOTPRINT_KB_MAX))
# The major-release/deep-engine robustness gate spends ten minutes fuzzing all
# three targets in parallel and ten minutes in thrash. Other gates add modest
# wall-time overhead.
ROBUSTNESS_FUZZ_SECONDS ?= 600

chaos: build bin/fclaw_chaos_check
	CHAOS_ITERATIONS=$(CHAOS_ITERATIONS) CHAOS_SEED=$(CHAOS_SEED) \
		CHAOS_MODE=$(CHAOS_MODE) CHAOS_RAMDISK_MIB=$(CHAOS_RAMDISK_MIB) \
		./tests/chaos/run.sh

disk-full: build bin/fclaw_chaos_check
	CHAOS_MODE=filesystem CHAOS_SEED=$(CHAOS_SEED) \
		CHAOS_RAMDISK_MIB=$(CHAOS_RAMDISK_MIB) ./tests/chaos/run.sh

thrash: build bin/fclaw_integration_tests bin/fclaw_thrash_driver
	THRASH_DURATION_SECONDS=$(THRASH_DURATION_SECONDS) THRASH_RATE=$(THRASH_RATE) \
		THRASH_MIN_RATE=$(THRASH_MIN_RATE) THRASH_CONTEXTS=$(THRASH_CONTEXTS) \
		THRASH_PREFILL=$(THRASH_PREFILL) \
		THRASH_DRAIN_TIMEOUT_SECONDS=$(THRASH_DRAIN_TIMEOUT_SECONDS) \
		THRASH_RSS_KB_MAX=$(THRASH_RSS_KB_MAX) \
		THRASH_FOOTPRINT_KB_MAX=$(THRASH_EFFECTIVE_FOOTPRINT_KB_MAX) \
		./tests/thrash/run.sh

asan:
	$(MAKE) clean
	$(MAKE) test CFLAGS='$(SANITIZER_CFLAGS)' LDFLAGS='$(SANITIZER_LDFLAGS)'

ubsan:
	$(MAKE) clean
	$(MAKE) test CFLAGS='$(SANITIZER_CFLAGS)' LDFLAGS='$(SANITIZER_LDFLAGS)'

robustness:
	$(MAKE) asan
	$(MAKE) ubsan
	$(MAKE) clean
	$(MAKE) test-small-stack
	$(MAKE) fuzz FUZZ_SECONDS=$(ROBUSTNESS_FUZZ_SECONDS)
	$(MAKE) chaos
	$(MAKE) disk-full
	$(MAKE) thrash
	$(MAKE) clean
	$(MAKE) build
	@printf 'robustness: PASS asan ubsan test-small-stack fuzz chaos disk-full thrash\n'

live-smoke: build
	./tests/live_provider_smoke.sh

clean:
	rm -f bin/fclaw bin/fclaw_unit_tests bin/fclaw_integration_tests bin/fclaw_chaos_check bin/fclaw_thrash_driver bin/fclaw_local_client_probe
	rm -f $(FUZZ_BINS)
	rm -f bin/actioncore bin/llm-call bin/jf bin/fclaw_setup bin/fclaw_auth
	rm -f bin/.DS_Store

# Strip the production binary. `make metrics` copies and strips without
# modifying the developer binary and is the canonical release measurement.
# The dev build keeps symbols
# so backtraces from crashes are readable.
release: build
	strip bin/fclaw
	@printf '\nbin/fclaw (stripped):\n'
	@ls -la bin/fclaw
	@printf '\nsize bin/fclaw:\n'
	@size bin/fclaw 2>/dev/null || true

metrics: build
	./scripts/metrics.sh

public-check:
	./scripts/public_source_check.sh .

release-check:
	./scripts/release_check.sh

help:
	@printf '%s\n' \
	  'make build          Build bin/fclaw' \
	  'make test           Run the routine hermetic release gate' \
	  'make metrics        Print reproducible release measurements' \
	  'make public-check   Check a public source checkout' \
	  'make release-check  Assemble and fully test the public candidate' \
	  'make robustness     Run the long major-release/deep-engine gate' \
	  'make clean          Remove generated binaries'

.PHONY: all build unit integration test test-small-stack asan ubsan fuzz-build fuzz chaos disk-full thrash robustness live-smoke release metrics public-check release-check help clean
