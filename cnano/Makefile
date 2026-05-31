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
LDLIBS  := -lm   # math.h (sqrt/pow/floor/...) in the builtins

SRC_DIR := src
BUILD   := build
SOURCES := $(wildcard $(SRC_DIR)/*.c)
TARGET  := $(BUILD)/cnano

.PHONY: all run test bench debug gcstress clean

all: $(TARGET)

$(TARGET): $(SOURCES) | $(BUILD)
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET) $(LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

run: all
	./$(TARGET)

# A separate debug build wired with AddressSanitizer + UBSan. Run your programs
# through this when something misbehaves; it pinpoints memory and UB bugs.
debug: | $(BUILD)
	$(CC) $(DBGFLAGS) $(SOURCES) -o $(BUILD)/cnano-debug $(LDLIBS)

test: all
	./tests/run_tests.sh ./$(TARGET)

# Run the self-timing benchmark suite (a stable baseline for VM/optimiser work).
bench: all
	./$(TARGET) bench/bench.cn

# GC STRESS build: collect on EVERY allocation, with sanitizers. Running the full
# suite through this is the real test of the collector — if any root is missed,
# a live object gets freed and ASan reports a use-after-free. Identical output to
# the normal build means the GC never reclaims something still in use.
gcstress: | $(BUILD)
	$(CC) $(DBGFLAGS) -DDEBUG_STRESS_GC $(SOURCES) -o $(BUILD)/cnano-gcstress $(LDLIBS)
	./tests/run_tests.sh ./$(BUILD)/cnano-gcstress

clean:
	rm -rf $(BUILD)
