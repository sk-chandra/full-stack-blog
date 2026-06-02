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

static void runFile(const char *path, bool trace) {
  // interpretFile() reads the file and resolves its imports relative to its dir.
  InterpretResult result = interpretFile(path, trace);

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

// `--emit-c FILE` : print the generated C for the typed subset to stdout, for
// inspection. No compiler is invoked.
static void emitCFile(const char *path) {
  InterpretResult r = compileFileToC(path, stdout);
  if (r != INTERPRET_OK)
    exit(65);
}

// `--native FILE -o OUT` : compile FILE to a native executable OUT, by emitting C
// to a temp file and invoking the system C compiler on it. This is the AOT path —
// the produced binary runs with no cnano runtime at all.
static void compileNative(const char *path, const char *outBinary) {
  // Write the generated C to a temporary file next to the output.
  char cPath[1024];
  snprintf(cPath, sizeof(cPath), "%s.c", outBinary);
  FILE *cFile = fopen(cPath, "wb");
  if (cFile == NULL) {
    fprintf(stderr, "cnano: cannot write \"%s\".\n", cPath);
    exit(74);
  }
  InterpretResult r = compileFileToC(path, cFile);
  fclose(cFile);
  if (r != INTERPRET_OK) {
    remove(cPath);
    exit(65);
  }

  // Invoke the system C compiler. We inherit its optimiser and target — the whole
  // point of transpiling to C. -O2 so the generated binary is genuinely fast.
  char cmd[2048];
  // -lm: the generated C uses <math.h> (sqrt/pow/… and isnan/isinf) for floats.
  snprintf(cmd, sizeof(cmd), "cc -O2 -o \"%s\" \"%s\" -lm", outBinary, cPath);
  int rc = system(cmd);
  if (rc != 0) {
    fprintf(stderr, "cnano: the C compiler failed on generated code.\n");
    exit(70);
  }
  remove(cPath); // clean up the intermediate C; keep only the binary
  fprintf(stderr, "cnano: wrote native executable \"%s\".\n", outBinary);
}

int main(int argc, const char *argv[]) {
  initVM();

  if (argc == 1) {
    repl();
  } else if (argc == 2) {
    runFile(argv[1], false);
  } else if (argc == 3 && strcmp(argv[1], "--dump") == 0) {
    runFile(argv[2], true);
  } else if (argc == 3 && strcmp(argv[1], "--emit-c") == 0) {
    emitCFile(argv[2]);
  } else if (argc == 3 && strcmp(argv[1], "--cfg") == 0) {
    if (dumpCFGFile(argv[2]) != INTERPRET_OK)
      exit(65);
  } else if (argc == 3 && strcmp(argv[1], "--ir") == 0) {
    if (dumpIRFile(argv[2]) != INTERPRET_OK)
      exit(65);
  } else if (argc == 3 && strcmp(argv[1], "--types") == 0) {
    if (dumpTypesFile(argv[2]) != INTERPRET_OK)
      exit(65);
  } else if (argc == 3 && strcmp(argv[1], "--asm") == 0) {
    if (dumpAsmFile(argv[2]) != INTERPRET_OK)
      exit(65);
  } else if (argc == 3 && strcmp(argv[1], "--stats") == 0) {
    // Profile a run: tally instructions/allocations/GC, then print a summary.
    vm.collectStats = true;
    runFile(argv[2], false);
    printVmStats();
  } else if (argc == 5 && strcmp(argv[1], "--native") == 0 &&
             strcmp(argv[3], "-o") == 0) {
    compileNative(argv[2], argv[4]);
  } else {
    fprintf(stderr,
            "Usage:\n"
            "  cnano [path]              run a file on the VM (default)\n"
            "  cnano                     start the REPL\n"
            "  cnano --dump path         run, showing bytecode + a VM trace\n"
            "  cnano --emit-c path       print generated C (typed subset)\n"
            "  cnano --cfg path          print each function's control-flow graph\n"
            "  cnano --ir path           lower straight-line code to IR and print it\n"
            "  cnano --types path        print the inferred type of each top-level binding\n"
            "  cnano --asm path          emit x86-64 assembly for straight-line integer code\n"
            "  cnano --stats path        run, then print a profiling summary\n"
            "  cnano --native path -o X  compile to a native executable X\n");
    freeVM();
    exit(64); // 64 = EX_USAGE
  }

  freeVM();
  return 0;
}
