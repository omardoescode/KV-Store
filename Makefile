CC := gcc

BUILD_DIR := build
TARGET := $(BUILD_DIR)/app
# cp with the Docker driver (production) and with the process driver (tests).
CP_TARGET := $(BUILD_DIR)/kv-cp
CP_PROC_TARGET := $(BUILD_DIR)/kv-cp-proc
COMPDB := $(BUILD_DIR)/compile_commands.json

CFLAGS := -std=c17 \
		  -D_GNU_SOURCE \
		  -pthread \
          -Wall -Wextra -Wpedantic \
          -Wshadow -Wconversion \
          -O2 -g -flto=auto

H2O_DIR := third_party/h2o
H2O_BUILD := $(BUILD_DIR)/h2o
H2O_LIB := $(H2O_BUILD)/libh2o-evloop.a
H2O_DEPS := klib picotls/include quicly/include yoml picohttpparser

YYJSON_DIR := third_party/yyjson
YYJSON_OBJ := $(BUILD_DIR)/yyjson.o
YYJSON_CPPFLAGS := -isystem $(YYJSON_DIR)/src

SIPHASH_DIR := third_party/siphash
SIPHASH_OBJ := $(BUILD_DIR)/siphash.o
SIPHASH_CPPFLAGS := -isystem $(SIPHASH_DIR)

SQLITE_LIBS := $(shell pkg-config --libs sqlite3 2>/dev/null || echo -lsqlite3)

CPPFLAGS := -Isrc

H2O_CPPFLAGS := -isystem $(H2O_DIR)/include $(H2O_DEPS:%=-isystem $(H2O_DIR)/deps/%) \
                -DH2O_USE_LIBUV=0 -D_GNU_SOURCE \
                $(shell pkg-config --cflags openssl zlib 2>/dev/null)

LDLIBS := $(YYJSON_OBJ) $(SIPHASH_OBJ) $(H2O_LIB) \
          $(shell pkg-config --libs openssl zlib 2>/dev/null || echo -lssl -lcrypto -lz) \
          $(SQLITE_LIBS) -lm

SRC := $(wildcard src/*.c src/*/*.c)
NODE_SRC := $(filter-out src/cp/%,$(SRC))
CP_SRC := $(filter-out src/main.c src/kv/% src/cp/driver_%,$(SRC))
OBJ := $(NODE_SRC:src/%.c=$(BUILD_DIR)/%.o)
CP_OBJ := $(CP_SRC:src/%.c=$(BUILD_DIR)/%.o)
DEP := $(SRC:src/%.c=$(BUILD_DIR)/%.d)

FMT_FILES := $(SRC) $(wildcard src/*.h src/*/*.h tests/*.c)

.PHONY: all clean distclean run test test-hashmap test-router test-log test-config test-kv-store test-vclock test-ring test-sm test-cluster test-resolve test-controller test-cp test-e2e test-shutdown memcheck bench bench-cluster format format-check lint lint-fix init

all: $(COMPDB) $(TARGET) $(if $(wildcard src/cp/main.c),$(CP_TARGET) $(CP_PROC_TARGET))

$(COMPDB): Makefile $(SRC)
	@mkdir -p $(dir $@)
	@{ echo '['; sep=''; for f in $(SRC); do \
		case $$f in \
			src/http/*) lib='$(H2O_CPPFLAGS)';; \
			src/json/*) lib='$(YYJSON_CPPFLAGS)';; \
			src/hashmap/*|src/keypos/*) lib='$(SIPHASH_CPPFLAGS)';; \
			*) lib='';; \
		esac; \
		printf '%s\n{"directory":"%s","file":"%s","command":"$(CC) $(CPPFLAGS) %s $(CFLAGS) -c %s"}' \
			"$$sep" "$(CURDIR)" "$$f" "$$lib" "$$f"; sep=','; \
	done; echo; echo ']'; } > $@

$(TARGET): $(OBJ) $(YYJSON_OBJ) $(SIPHASH_OBJ) $(H2O_LIB)
	$(CC) $(CFLAGS) $(OBJ) $(LDLIBS) -o $@

$(CP_TARGET): $(CP_OBJ) $(BUILD_DIR)/cp/driver_docker.o $(YYJSON_OBJ) $(SIPHASH_OBJ) $(H2O_LIB)
	$(CC) $(CFLAGS) $(CP_OBJ) $(BUILD_DIR)/cp/driver_docker.o $(LDLIBS) -o $@

$(CP_PROC_TARGET): $(CP_OBJ) $(BUILD_DIR)/cp/driver_process.o $(YYJSON_OBJ) $(SIPHASH_OBJ) $(H2O_LIB)
	$(CC) $(CFLAGS) $(CP_OBJ) $(BUILD_DIR)/cp/driver_process.o $(LDLIBS) -o $@

$(H2O_DIR)/CMakeLists.txt:
	git submodule update --init --depth 1 $(H2O_DIR)

$(YYJSON_DIR)/src/yyjson.c:
	git submodule update --init --depth 1 $(YYJSON_DIR)

$(YYJSON_OBJ): $(YYJSON_DIR)/src/yyjson.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O2 -g -flto=auto -c $< -o $@

$(SIPHASH_DIR)/siphash.c:
	git submodule update --init --depth 1 $(SIPHASH_DIR)

$(SIPHASH_OBJ): $(SIPHASH_DIR)/siphash.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O2 -g -flto=auto -DcROUNDS=1 -DdROUNDS=3 -c $< -o $@

$(H2O_LIB): $(H2O_DIR)/CMakeLists.txt
	cmake -S $(H2O_DIR) -B $(H2O_BUILD) -DCMAKE_BUILD_TYPE=Release \
		-DWITH_MRUBY=OFF -DWITH_DTRACE=OFF -DWITH_BROTLI=OFF -DWITH_ZSTD=OFF \
		-DWITH_AEGIS=OFF -DWITH_CCACHE=OFF -DWITH_IO_URING=OFF
	# --parallel sets its own -j; the jobserver of an outer make -j would clash with it.
	MAKEFLAGS= cmake --build $(H2O_BUILD) --target libh2o-evloop --parallel

$(BUILD_DIR)/http/%.o: CPPFLAGS += $(H2O_CPPFLAGS)
$(BUILD_DIR)/json/%.o: CPPFLAGS += $(YYJSON_CPPFLAGS)
$(BUILD_DIR)/hashmap/%.o: CPPFLAGS += $(SIPHASH_CPPFLAGS)
$(BUILD_DIR)/keypos/%.o: CPPFLAGS += $(SIPHASH_CPPFLAGS)
$(BUILD_DIR)/cp/%.o: CPPFLAGS += $(SIPHASH_CPPFLAGS) $(H2O_CPPFLAGS) $(YYJSON_CPPFLAGS)

$(BUILD_DIR)/%.o: src/%.c | $(H2O_DIR)/CMakeLists.txt $(YYJSON_DIR)/src/yyjson.c $(SIPHASH_DIR)/siphash.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEP)

run: $(TARGET)
	@[ -f ./.env ] || { echo "run: ./.env not found; run 'cp .env.example .env'" >&2; exit 1; }
	@mkdir -p in
	@set -a; . ./.env; set +a; exec ./$(TARGET)

HM_TEST_SRC := src/hashmap/hashmap.c tests/hashmap_test.c
HM_TEST_BIN := $(BUILD_DIR)/hashmap_test

$(HM_TEST_BIN): $(HM_TEST_SRC) src/hashmap/hashmap.h $(SIPHASH_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(SIPHASH_CPPFLAGS) $(CFLAGS) $(HM_TEST_SRC) $(SIPHASH_OBJ) -o $@

test-hashmap: $(HM_TEST_BIN)
	./$(HM_TEST_BIN)

ROUTER_TEST_SRC := src/http/router.c tests/http_router_test.c
ROUTER_TEST_BIN := $(BUILD_DIR)/http_router_test

$(ROUTER_TEST_BIN): $(ROUTER_TEST_SRC) src/http/router.h src/http/http.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ROUTER_TEST_SRC) -o $@

test-router: $(ROUTER_TEST_BIN)
	./$(ROUTER_TEST_BIN)

LOG_TEST_SRC := src/log/log_ring.c src/log/async_log.c tests/log_test.c
LOG_TEST_BIN := $(BUILD_DIR)/log_test

$(LOG_TEST_BIN): $(LOG_TEST_SRC) src/log/log.h src/log/log_ring.h src/log/async_log.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LOG_TEST_SRC) -o $@

test-log: $(LOG_TEST_BIN)
	./$(LOG_TEST_BIN)

CONFIG_TEST_SRC := src/config/config.c tests/config_test.c
CONFIG_TEST_BIN := $(BUILD_DIR)/config_test

$(CONFIG_TEST_BIN): $(CONFIG_TEST_SRC) src/config/config.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CONFIG_TEST_SRC) -o $@

test-config: $(CONFIG_TEST_BIN)
	./$(CONFIG_TEST_BIN)

KV_STORE_TEST_SRC := src/kv/kv_store.c src/hashmap/hashmap.c src/vclock/vclock.c \
                     src/keypos/keypos.c tests/kv_store_test.c
KV_STORE_TEST_BIN := $(BUILD_DIR)/kv_store_test

$(KV_STORE_TEST_BIN): $(KV_STORE_TEST_SRC) src/kv/kv_store.h src/vclock/vclock.h src/hashmap/hashmap.h src/log/log.h $(SIPHASH_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(SIPHASH_CPPFLAGS) $(CFLAGS) $(KV_STORE_TEST_SRC) $(SIPHASH_OBJ) $(SQLITE_LIBS) -o $@

test-kv-store: $(KV_STORE_TEST_BIN)
	./$(KV_STORE_TEST_BIN)

VCLOCK_TEST_SRC := src/vclock/vclock.c tests/vclock_test.c
VCLOCK_TEST_BIN := $(BUILD_DIR)/vclock_test

$(VCLOCK_TEST_BIN): $(VCLOCK_TEST_SRC) src/vclock/vclock.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VCLOCK_TEST_SRC) -o $@

test-vclock: $(VCLOCK_TEST_BIN)
	./$(VCLOCK_TEST_BIN)

RING_TEST_SRC := src/cp/ring.c src/keypos/keypos.c tests/ring_test.c
RING_TEST_BIN := $(BUILD_DIR)/ring_test

$(RING_TEST_BIN): $(RING_TEST_SRC) src/cp/ring.h $(SIPHASH_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(SIPHASH_CPPFLAGS) $(CFLAGS) $(RING_TEST_SRC) $(SIPHASH_OBJ) -o $@

test-ring: $(RING_TEST_BIN)
	./$(RING_TEST_BIN)

SM_TEST_SRC := src/cp/sm.c tests/cp_sm_test.c
SM_TEST_BIN := $(BUILD_DIR)/cp_sm_test

$(SM_TEST_BIN): $(SM_TEST_SRC) src/cp/sm.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SM_TEST_SRC) -o $@

test-sm: $(SM_TEST_BIN)
	./$(SM_TEST_BIN)

CLUSTER_TEST_SRC := src/cp/cluster.c src/cp/ring.c src/keypos/keypos.c src/cp/sm.c src/cp/store.c \
                    tests/cluster_test.c
CLUSTER_TEST_BIN := $(BUILD_DIR)/cluster_test

$(CLUSTER_TEST_BIN): $(CLUSTER_TEST_SRC) src/cp/cluster.h src/cp/ring.h src/cp/sm.h src/cp/store.h $(SIPHASH_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(SIPHASH_CPPFLAGS) $(CFLAGS) $(CLUSTER_TEST_SRC) $(SIPHASH_OBJ) $(SQLITE_LIBS) -o $@

test-cluster: $(CLUSTER_TEST_BIN)
	./$(CLUSTER_TEST_BIN)

RESOLVE_TEST_SRC := src/cp/resolve.c tests/resolve_test.c
RESOLVE_TEST_BIN := $(BUILD_DIR)/resolve_test

$(RESOLVE_TEST_BIN): $(RESOLVE_TEST_SRC) src/cp/resolve.h src/cp/driver.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RESOLVE_TEST_SRC) -o $@

test-resolve: $(RESOLVE_TEST_BIN)
	./$(RESOLVE_TEST_BIN)

CTL_TEST_SRC := src/cp/controller.c src/cp/nodes.c src/cp/cluster.c \
                src/cp/ring.c src/keypos/keypos.c src/cp/sm.c src/cp/store.c src/vclock/vclock.c \
                tests/node_client_fake.c tests/driver_fake.c \
                tests/controller_test.c
CTL_TEST_BIN := $(BUILD_DIR)/controller_test

$(CTL_TEST_BIN): $(CTL_TEST_SRC) $(wildcard src/cp/*.h) tests/cp_fake.h $(SIPHASH_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(SIPHASH_CPPFLAGS) $(CFLAGS) $(CTL_TEST_SRC) $(SIPHASH_OBJ) $(SQLITE_LIBS) -o $@

test-controller: $(CTL_TEST_BIN)
	./$(CTL_TEST_BIN)

TEST_BINS := $(VCLOCK_TEST_BIN) $(HM_TEST_BIN) $(RING_TEST_BIN) $(SM_TEST_BIN) $(CLUSTER_TEST_BIN) $(RESOLVE_TEST_BIN) $(CTL_TEST_BIN) $(ROUTER_TEST_BIN) $(LOG_TEST_BIN) $(CONFIG_TEST_BIN) $(KV_STORE_TEST_BIN)

test-cp: $(TARGET) $(CP_PROC_TARGET)
	python3 tests/cp_test.py

E2E_JOBS ?=
E2E_SHARD ?= 1/1

test-e2e: $(TARGET) $(CP_PROC_TARGET)
	python3 tests/e2e.py $(if $(E2E_JOBS),-j $(E2E_JOBS)) --shard $(E2E_SHARD)

test-shutdown: $(TARGET)
	tests/shutdown_test.sh

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do ./$$t; done

memcheck: $(TARGET) $(CP_PROC_TARGET)
	tests/memcheck.sh

bench:
	python3 bench/profile.py $(BENCH_ARGS)

bench-cluster:
	python3 bench/cluster_profile.py $(BENCH_ARGS)

CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy

format:
	$(CLANG_FORMAT) -i $(FMT_FILES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FMT_FILES)

lint:
	$(CLANG_TIDY) --quiet $(TIDYFLAGS) $(SRC) -- $(CPPFLAGS) $(H2O_CPPFLAGS) $(YYJSON_CPPFLAGS) $(SIPHASH_CPPFLAGS) $(CFLAGS)

lint-fix:
	$(CLANG_TIDY) --quiet --fix $(TIDYFLAGS) $(SRC) -- $(CPPFLAGS) $(H2O_CPPFLAGS) $(YYJSON_CPPFLAGS) $(SIPHASH_CPPFLAGS) $(CFLAGS)

init:
	@command -v gcc >/dev/null || { echo "init: gcc not installed" >&2; exit 1; }
	@command -v $(CLANG_FORMAT) >/dev/null || { echo "init: clang-format not installed" >&2; exit 1; }
	@command -v $(CLANG_TIDY) >/dev/null || { echo "init: clang-tidy not installed" >&2; exit 1; }
	@command -v cmake >/dev/null || { echo "init: cmake not installed" >&2; exit 1; }
	@command -v c++ >/dev/null || { echo "init: C++ compiler not installed (H2O build)" >&2; exit 1; }
	@command -v pkg-config >/dev/null || { echo "init: pkg-config not installed" >&2; exit 1; }
	@command -v valgrind >/dev/null || { echo "init: valgrind not installed" >&2; exit 1; }
	@command -v curl >/dev/null || { echo "init: curl not installed" >&2; exit 1; }
	@command -v sqlite3 >/dev/null || { echo "init: sqlite3 CLI not installed" >&2; exit 1; }
	@pkg-config --exists openssl zlib sqlite3 || { echo "init: openssl, zlib or sqlite3 dev files not installed" >&2; exit 1; }
	git submodule update --init --depth 1
	git config core.hooksPath .githooks

clean:
	rm -f $(TARGET) $(CP_TARGET) $(CP_PROC_TARGET) $(OBJ) $(CP_OBJ) $(DEP) $(YYJSON_OBJ) $(SIPHASH_OBJ) $(TEST_BINS)

distclean:
	rm -rf $(BUILD_DIR)
