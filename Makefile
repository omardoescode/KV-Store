CC := gcc

BUILD_DIR := build
TARGET := $(BUILD_DIR)/app
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
H2O_DEPS := klib picotls/include quicly/include yoml

YYJSON_DIR := third_party/yyjson
YYJSON_OBJ := $(BUILD_DIR)/yyjson.o
YYJSON_CPPFLAGS := -isystem $(YYJSON_DIR)/src

SIPHASH_DIR := third_party/siphash
SIPHASH_OBJ := $(BUILD_DIR)/siphash.o
SIPHASH_CPPFLAGS := -isystem $(SIPHASH_DIR)

CPPFLAGS := -Isrc

H2O_CPPFLAGS := -isystem $(H2O_DIR)/include $(H2O_DEPS:%=-isystem $(H2O_DIR)/deps/%) \
                -DH2O_USE_LIBUV=0 -D_GNU_SOURCE \
                $(shell pkg-config --cflags openssl zlib 2>/dev/null)

LDLIBS := $(YYJSON_OBJ) $(SIPHASH_OBJ) $(H2O_LIB) \
          $(shell pkg-config --libs openssl zlib 2>/dev/null || echo -lssl -lcrypto -lz) \
          -lm

SRC := $(wildcard src/*.c src/*/*.c)
OBJ := $(SRC:src/%.c=$(BUILD_DIR)/%.o)
DEP := $(OBJ:.o=.d)

FMT_FILES := $(SRC) $(wildcard src/*.h src/*/*.h tests/*.c)

.PHONY: all clean distclean run test test-hashmap test-router test-log memcheck format format-check lint lint-fix init

all: $(COMPDB) $(TARGET)

$(COMPDB): Makefile $(SRC)
	@mkdir -p $(dir $@)
	@{ echo '['; sep=''; for f in $(SRC); do \
		case $$f in \
			src/http/*) lib='$(H2O_CPPFLAGS)';; \
			src/json/*) lib='$(YYJSON_CPPFLAGS)';; \
			src/hashmap/*) lib='$(SIPHASH_CPPFLAGS)';; \
			*) lib='';; \
		esac; \
		printf '%s\n{"directory":"%s","file":"%s","command":"$(CC) $(CPPFLAGS) %s $(CFLAGS) -c %s"}' \
			"$$sep" "$(CURDIR)" "$$f" "$$lib" "$$f"; sep=','; \
	done; echo; echo ']'; } > $@

$(TARGET): $(OBJ) $(YYJSON_OBJ) $(SIPHASH_OBJ) $(H2O_LIB)
	$(CC) $(CFLAGS) $(OBJ) $(LDLIBS) -o $@

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
	cmake --build $(H2O_BUILD) --target libh2o-evloop --parallel

$(BUILD_DIR)/http/%.o: CPPFLAGS += $(H2O_CPPFLAGS)
$(BUILD_DIR)/json/%.o: CPPFLAGS += $(YYJSON_CPPFLAGS)
$(BUILD_DIR)/hashmap/%.o: CPPFLAGS += $(SIPHASH_CPPFLAGS)

$(BUILD_DIR)/%.o: src/%.c | $(H2O_DIR)/CMakeLists.txt $(YYJSON_DIR)/src/yyjson.c $(SIPHASH_DIR)/siphash.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEP)

run: $(TARGET)
	./$(TARGET)

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

TEST_BINS := $(HM_TEST_BIN) $(ROUTER_TEST_BIN) $(LOG_TEST_BIN)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do ./$$t; done

memcheck: $(TARGET)
	tests/memcheck.sh

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
	@pkg-config --exists openssl zlib || { echo "init: openssl or zlib dev files not installed" >&2; exit 1; }
	git submodule update --init --depth 1
	git config core.hooksPath .githooks

clean:
	rm -f $(TARGET) $(OBJ) $(DEP) $(YYJSON_OBJ) $(SIPHASH_OBJ) $(TEST_BINS)

distclean:
	rm -rf $(BUILD_DIR)
