CC := gcc
CPPFLAGS ?= -Izat
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror -pedantic -Wconversion -Wshadow -Wstrict-prototypes
LDFLAGS ?=

BUILD_DIR := _build
TARGET := $(BUILD_DIR)/test_zat.exe
LOG := $(BUILD_DIR)/test.log
OBJECTS := $(BUILD_DIR)/zat.o $(BUILD_DIR)/test_zat.o
DEPS := $(OBJECTS:.o=.d)

export TMPDIR := $(abspath $(BUILD_DIR))
export TMP := $(TMPDIR)
export TEMP := $(TMPDIR)

.PHONY: all test clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/zat.o: zat/zat.c zat/zat.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_zat.o: test_zat.c zat/zat.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

test: $(TARGET)
	@{ echo "compiler: $$($(CC) --version | head -n 1)"; echo "binary: $(TARGET)"; $(TARGET); } > $(LOG) 2>&1; status=$$?; cat $(LOG); exit $$status

clean:
	$(RM) -r -- $(BUILD_DIR)

-include $(DEPS)
