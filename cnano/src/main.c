// main.c — the cnano command-line interface.
//
// Usage:
//   cnano                 start a REPL (read-eval-print loop)
//   cnano FILE.cn         compile and run a source file
//   cnano --dump FILE.cn  also print the bytecode and an execution trace
//
// This file wires the pieces together; all the interesting logic lives in the
// lexer/parser/compiler/vm. Keeping main.c thin is good practice: the CLI is
// just one possible "front door" to the same core.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"

// Read an entire file into a heap buffer (NUL-terminated). The caller frees it.
static char *readFile(const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "cnano: could not open file \"%s\".\n", path);
    exit(74); // 74 = EX_IOERR
  }

  // Read in chunks, growing a buffer as we go. We deliberately do NOT use
  // fseek/ftell to size the file up front: those fail on non-seekable inputs
  // like pipes and /dev/stdin (ftell returns -1), which previously caused a
  // buffer overflow. Chunked reading works for regular files AND streams.
  size_t capacity = 1024;
  size_t length = 0;
  char *buffer = malloc(capacity);
  if (buffer == NULL) {
    fprintf(stderr, "cnano: not enough memory to read \"%s\".\n", path);
    exit(74);
  }
  for (;;) {
    if (length + 1 >= capacity) { // leave room for the trailing '\0'
      capacity *= 2;
      buffer = realloc(buffer, capacity);
      if (buffer == NULL) {
        fprintf(stderr, "cnano: not enough memory to read \"%s\".\n", path);
        exit(74);
      }
    }
    size_t got = fread(buffer + length, 1, capacity - length - 1, file);
    length += got;
    if (got == 0)
      break; // EOF or error
  }
  buffer[length] = '\0';

  fclose(file);
  return buffer;
}

static void runFile(const char *path, bool trace) {
  char *source = readFile(path);
  InterpretResult result = interpret(source, trace);
  free(source);

  // Map interpreter outcomes onto conventional Unix exit codes so cnano plays
  // nicely in shell pipelines and test scripts.
  if (result == INTERPRET_COMPILE_ERROR)
    exit(65);
  if (result == INTERPRET_RUNTIME_ERROR)
    exit(70);
}

static void repl(void) {
  char line[1024];
  printf("cnano REPL — type statements, Ctrl-D to quit.\n");
  printf("  e.g.  print 1 + 2;     print 3 < 5;\n");
  printf("  (output only appears via `print`; statements end with ';')\n");
  for (;;) {
    printf("> ");
    if (!fgets(line, sizeof(line), stdin)) {
      printf("\n");
      break;
    }
    // Ignore errors in the REPL so one typo doesn't end the session.
    interpret(line, false);
  }
}

int main(int argc, const char *argv[]) {
  initVM();

  if (argc == 1) {
    repl();
  } else if (argc == 2) {
    runFile(argv[1], false);
  } else if (argc == 3 && strcmp(argv[1], "--dump") == 0) {
    runFile(argv[2], true);
  } else {
    fprintf(stderr, "Usage: cnano [--dump] [path]\n");
    freeVM();
    exit(64); // 64 = EX_USAGE
  }

  freeVM();
  return 0;
}
