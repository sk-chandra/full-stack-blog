# Makefile for cnano.
#
# `make`         build the optimised binary at build/cnano
# `make run`     build, then start the REPL
# `make test`    build, then run the test suite
# `make debug`   build with sanitizers + debug info (great for catching bugs)
# `make clean`   remove build artifacts
#
# The flags are deliberately strict: -Wall -Wextra catch the mistakes that bite
# C beginners hardest. -std=c11 picks a modern, portable C standard.

CC      := cc
CFLAGS  := -std=c11 -Wall -Wextra -O2
DBGFLAGS:= -std=c11 -Wall -Wextra -g -O0 -fsanitize=address,undefined

SRC_DIR := src
BUILD   := build
SOURCES := $(wildcard $(SRC_DIR)/*.c)
TARGET  := $(BUILD)/cnano

.PHONY: all run test debug clean

all: $(TARGET)

$(TARGET): $(SOURCES) | $(BUILD)
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

run: all
	./$(TARGET)

# A separate debug build wired with AddressSanitizer + UBSan. Run your programs
# through this when something misbehaves; it pinpoints memory and UB bugs.
debug: | $(BUILD)
	$(CC) $(DBGFLAGS) $(SOURCES) -o $(BUILD)/cnano-debug

test: all
	./tests/run_tests.sh ./$(TARGET)

clean:
	rm -rf $(BUILD)
