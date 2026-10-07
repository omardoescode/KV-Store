CC := gcc

BUILD_DIR := bin
TARGET := $(BUILD_DIR)/app

CFLAGS := -std=c17 \
          -Wall -Wextra -Wpedantic \
          -Wshadow -Wconversion \
          -O2 -g

CPPFLAGS := -Iinclude

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=$(BUILD_DIR)/%.o)
DEP := $(OBJ:.o=.d)

FMT_FILES := $(SRC) $(wildcard include/*.h) $(wildcard tests/*.c)

.PHONY: all clean test format format-check lint lint-fix hooks

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEP)

test: $(TARGET)
	./$(TARGET)

format:
	clang-format -i $(FMT_FILES)

format-check:
	clang-format --dry-run --Werror $(FMT_FILES)

lint:
	clang-tidy --quiet $(TIDYFLAGS) $(SRC) -- $(CPPFLAGS) $(CFLAGS)

lint-fix:
	clang-tidy --quiet --fix $(TIDYFLAGS) $(SRC) -- $(CPPFLAGS) $(CFLAGS)

hooks:
	git config core.hooksPath .githooks

clean:
	rm -rf $(BUILD_DIR)
