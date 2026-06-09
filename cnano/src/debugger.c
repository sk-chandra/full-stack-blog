// debugger.c — the interactive stepping debugger (step 77). See debugger.h for
// the two ideas (line-table watching + the compiler's debug-info table).
//
// Commands at the (cdb) prompt:
//   s / step          run to the next source line, stepping INTO calls
//   n / next          run to the next source line in THIS function or a caller
//   c / continue      run until the next breakpoint (or the end)
//   b N / break N     set a breakpoint at line N
//   p X / print X     print variable X (locals via debug info, then globals)
//   vars              list the locals in scope right here
//   bt / where        print the call stack
//   q / quit          stop the program
// End-of-input (e.g. a piped command file running out) acts like `continue`
// with the debugger switched off, so the program simply finishes.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debugger.h"
#include "module.h" // moduleFormatLine, CNANO_FILE_SPAN
#include "object.h"
#include "table.h"

Debugger debugger;

void debuggerStart(const char *path) {
  memset(&debugger, 0, sizeof(debugger));
  debugger.active = true;
  debugger.stepping = true; // pause on the very first line, so the user can set
  debugger.stepFrameDepth = INT_MAX; // breakpoints before anything runs
  debugger.currentLine = -1;

  // Keep the root file's text so a pause can SHOW the line it stopped on.
  FILE *f = fopen(path, "rb");
  if (f != NULL) {
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    debugger.source = malloc((size_t)size + 1);
    if (debugger.source != NULL) {
      size_t got = fread(debugger.source, 1, (size_t)size, f);
      debugger.source[got] = '\0';
    }
    fclose(f);
  }
}

void debuggerEnd(void) {
  free(debugger.source);
  debugger.source = NULL;
  debugger.active = false;
}

// Print line `line` of the root file's text (1-based, band 0 only — text of
// imported files isn't kept). Quietly does nothing if unavailable.
static void showSourceLine(int line) {
  if (debugger.source == NULL || line < 1 || line >= CNANO_FILE_SPAN)
    return;
  const char *p = debugger.source;
  for (int n = 1; n < line && *p; ) // skip to the start of `line`
    if (*p++ == '\n')
      n++;
  if (*p == '\0')
    return;
  printf("    ");
  while (*p && *p != '\n')
    putchar(*p++);
  printf("\n");
}

static bool isBreakpoint(int line) {
  for (int i = 0; i < debugger.breakpointCount; i++)
    if (debugger.breakpoints[i] == line)
      return true;
  return false;
}

// Look up `name` among the locals IN SCOPE at `offset` in this frame, using the
// compiler's debug-info table. Innermost wins (later records shadow earlier
// ones, mirroring how the compiler resolves names back-to-front).
static bool findLocal(CallFrame *frame, const char *name, int offset,
                      Value *out) {
  Chunk *chunk = &frame->closure->function->chunk;
  for (int i = chunk->debugLocalCount - 1; i >= 0; i--) {
    LocalDebug *d = &chunk->debugLocals[i];
    bool inRange = offset >= d->startOffset &&
                   (d->endOffset < 0 || offset < d->endOffset);
    if (inRange && d->name != NULL && strcmp(d->name->chars, name) == 0) {
      *out = frame->slots[d->slot];
      return true;
    }
  }
  return false;
}

// Print variable `name`: locals (via debug info) first, then globals. The
// globals table is scanned by string content so nothing is allocated mid-pause.
static void printVariable(CallFrame *frame, const char *name, int offset) {
  Value v;
  if (findLocal(frame, name, offset, &v)) {
    printf("%s = ", name);
    printValue(v);
    printf("\n");
    return;
  }
  for (int i = 0; i < vm.globals.capacity; i++) {
    Entry *e = &vm.globals.entries[i];
    if (e->key != NULL && strcmp(e->key->chars, name) == 0) {
      printf("%s = ", name);
      printValue(e->value);
      printf("\n");
      return;
    }
  }
  printf("no variable named '%s' is in scope\n", name);
}

// List every local in scope at this pause (what the debug-info table knows).
static void listLocals(CallFrame *frame, int offset) {
  Chunk *chunk = &frame->closure->function->chunk;
  bool any = false;
  for (int i = 0; i < chunk->debugLocalCount; i++) {
    LocalDebug *d = &chunk->debugLocals[i];
    bool inRange = offset >= d->startOffset &&
                   (d->endOffset < 0 || offset < d->endOffset);
    if (!inRange || d->name == NULL)
      continue;
    printf("  %s = ", d->name->chars);
    printValue(frame->slots[d->slot]);
    printf("\n");
    any = true;
  }
  if (!any)
    printf("  (no locals in scope)\n");
}

// The call stack, innermost first. The paused frame's ip points AT the next
// instruction; caller frames' ips point just PAST their call, hence the -1.
static void backtrace(void) {
  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame *f = &vm.frames[i];
    ObjFunction *fn = f->closure->function;
    size_t offset = (size_t)(f->ip - fn->chunk.code);
    if (i != vm.frameCount - 1)
      offset--;
    char loc[128];
    moduleFormatLine(fn->chunk.lines[offset], loc, sizeof(loc));
    printf("  #%d %s (%s)\n", vm.frameCount - 1 - i,
           fn->name != NULL ? fn->name->chars : "<script>", loc);
  }
}

// The interactive prompt. Returns when execution should resume.
static void pause(CallFrame *frame, int line, int offset, bool atBreakpoint) {
  char loc[128];
  moduleFormatLine(line, loc, sizeof(loc));
  printf("[debug] %s at %s\n", atBreakpoint ? "breakpoint" : "stopped", loc);
  showSourceLine(line);

  char buf[256];
  for (;;) {
    printf("(cdb) ");
    fflush(stdout);
    if (fgets(buf, sizeof(buf), stdin) == NULL) {
      // Out of input: behave like `continue`, and stop pausing for good.
      debugger.active = false;
      debugger.stepping = false;
      return;
    }
    buf[strcspn(buf, "\n")] = '\0';
    char *cmd = strtok(buf, " \t");
    char *arg = strtok(NULL, " \t");
    if (cmd == NULL)
      continue;

    if (strcmp(cmd, "s") == 0 || strcmp(cmd, "step") == 0) {
      debugger.stepping = true;
      debugger.stepFrameDepth = INT_MAX; // any depth: steps INTO calls
      debugger.stepFromLine = -1;
      return;
    }
    if (strcmp(cmd, "n") == 0 || strcmp(cmd, "next") == 0) {
      debugger.stepping = true;
      debugger.stepFrameDepth = vm.frameCount; // this depth or shallower only
      debugger.stepFromLine = line; // don't re-pause on this line after a call
      return;
    }
    if (strcmp(cmd, "c") == 0 || strcmp(cmd, "continue") == 0) {
      debugger.stepping = false;
      return;
    }
    if ((strcmp(cmd, "b") == 0 || strcmp(cmd, "break") == 0) && arg != NULL) {
      if (debugger.breakpointCount <
          (int)(sizeof(debugger.breakpoints) / sizeof(int))) {
        int bp = atoi(arg);
        debugger.breakpoints[debugger.breakpointCount++] = bp;
        printf("breakpoint set at line %d\n", bp);
      }
      continue;
    }
    if ((strcmp(cmd, "p") == 0 || strcmp(cmd, "print") == 0) && arg != NULL) {
      printVariable(frame, arg, offset);
      continue;
    }
    if (strcmp(cmd, "vars") == 0) {
      listLocals(frame, offset);
      continue;
    }
    if (strcmp(cmd, "bt") == 0 || strcmp(cmd, "where") == 0) {
      backtrace();
      continue;
    }
    if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0) {
      printf("quit\n");
      exit(0);
    }
    printf("commands: s(tep)  n(ext)  c(ontinue)  b LINE  p NAME  vars  bt  q\n");
  }
}

void debuggerHook(CallFrame *frame) {
  Chunk *chunk = &frame->closure->function->chunk;
  int offset = (int)(frame->ip - chunk->code);
  int line = chunk->lines[offset];
  if (line == debugger.currentLine)
    return; // still inside the same source line — not a step
  debugger.currentLine = line;

  bool bp = isBreakpoint(line);
  bool step = debugger.stepping && vm.frameCount <= debugger.stepFrameDepth &&
              // `next` must not re-pause on its own line after a call WITHIN the
              // line returns (same frame + same line = still the same statement;
              // a loop iteration reaches a DIFFERENT line first, so it still stops)
              !(line == debugger.stepFromLine &&
                vm.frameCount == debugger.stepFrameDepth);
  if (bp || step)
    pause(frame, line, offset, bp && !step);
}
