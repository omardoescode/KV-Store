CC := gcc

BUILD_DIR := build
TARGET := $(BUILD_DIR)/app
COMPDB := $(BUILD_DIR)/compile_commands.json

CFLAGS := -std=c17 \
          -Wall -Wextra -Wpedantic \
          -Wshadow -Wconversion \
          -O2 -g -flto

H2O_DIR := third_party/h2o
H2O_BUILD := $(BUILD_DIR)/h2o
H2O_LIB := $(H2O_BUILD)/libh2o-evloop.a
H2O_DEPS := klib picotls/include quicly/include yoml

CPPFLAGS := -Iinclude -Isrc

H2O_CPPFLAGS := -isystem $(H2O_DIR)/include $(H2O_DEPS:%=-isystem $(H2O_DIR)/deps/%) \
                -DH2O_USE_LIBUV=0 -D_GNU_SOURCE \
                $(shell pkg-config --cflags openssl zlib 2>/dev/null)

LDLIBS := $(H2O_LIB) \
          $(shell pkg-config --libs openssl zlib 2>/dev/null || echo -lssl -lcrypto -lz) \
          -lm -pthread

SRC := $(wildcard src/*.c src/*/*.c)
OBJ := $(SRC:src/%.c=$(BUILD_DIR)/%.o)
DEP := $(OBJ:.o=.d)

FMT_FILES := $(SRC) $(wildcard src/*.h src/*/*.h include/*.h tests/*.c)

.PHONY: all clean distclean test format format-check lint lint-fix init

all: $(COMPDB) $(TARGET)

$(COMPDB): Makefile $(SRC)
	@mkdir -p $(dir $@)
	@{ echo '['; sep=''; for f in $(SRC); do \
		case $$f in src/http/*) h2o='$(H2O_CPPFLAGS)';; *) h2o='';; esac; \
		printf '%s\n{"directory":"%s","file":"%s","command":"$(CC) $(CPPFLAGS) %s $(CFLAGS) -c %s"}' \
			"$$sep" "$(CURDIR)" "$$f" "$$h2o" "$$f"; sep=','; \
	done; echo; echo ']'; } > $@

$(TARGET): $(OBJ) $(H2O_LIB)
	$(CC) $(CFLAGS) $(OBJ) $(LDLIBS) -o $@

$(H2O_DIR)/CMakeLists.txt:
	git submodule update --init --depth 1 $(H2O_DIR)

$(H2O_LIB): $(H2O_DIR)/CMakeLists.txt
	cmake -S $(H2O_DIR) -B $(H2O_BUILD) -DCMAKE_BUILD_TYPE=Release \
		-DWITH_MRUBY=OFF -DWITH_DTRACE=OFF -DWITH_BROTLI=OFF -DWITH_ZSTD=OFF \
		-DWITH_AEGIS=OFF -DWITH_CCACHE=OFF -DWITH_IO_URING=OFF
	cmake --build $(H2O_BUILD) --target libh2o-evloop --parallel

$(BUILD_DIR)/http/%.o: CPPFLAGS += $(H2O_CPPFLAGS)

$(BUILD_DIR)/%.o: src/%.c | $(H2O_DIR)/CMakeLists.txt
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEP)

test: $(TARGET)
	./$(TARGET)

format:
	clang-format -i $(FMT_FILES)

format-check:
	clang-format --dry-run --Werror $(FMT_FILES)

lint:
	clang-tidy --quiet $(TIDYFLAGS) $(SRC) -- $(CPPFLAGS) $(H2O_CPPFLAGS) $(CFLAGS)

lint-fix:
	clang-tidy --quiet --fix $(TIDYFLAGS) $(SRC) -- $(CPPFLAGS) $(H2O_CPPFLAGS) $(CFLAGS)

init:
	@command -v gcc >/dev/null || { echo "init: gcc not installed" >&2; exit 1; }
	@command -v clang-format >/dev/null || { echo "init: clang-format not installed" >&2; exit 1; }
	@command -v clang-tidy >/dev/null || { echo "init: clang-tidy not installed" >&2; exit 1; }
	@command -v cmake >/dev/null || { echo "init: cmake not installed" >&2; exit 1; }
	@command -v pkg-config >/dev/null || { echo "init: pkg-config not installed" >&2; exit 1; }
	@pkg-config --exists openssl zlib || { echo "init: openssl or zlib dev files not installed" >&2; exit 1; }
	git submodule update --init --depth 1
	git config core.hooksPath .githooks

clean:
	rm -f $(TARGET) $(OBJ) $(DEP)

distclean:
	rm -rf $(BUILD_DIR)
