CC := cc
CFLAGS := -O2 -Wall -Wextra -std=c11
BUILD := build
TARGET := $(BUILD)/wc

all: $(TARGET)

$(TARGET): src/wc.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: $(TARGET)
	./$(TARGET) src/wc.c

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
