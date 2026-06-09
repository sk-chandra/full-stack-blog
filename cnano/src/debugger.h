// debugger.h — an interactive stepping debugger for the bytecode VM (step 77).
//
// `cnano --debug file.cn` runs the program under the debugger: it pauses on the
// first line, then takes commands from stdin — step, continue, breakpoints,
// printing variables, a backtrace. Two ideas make it work:
//
//   1. The VM already knows which SOURCE LINE each instruction came from (the
//      chunk's line table, kept for error messages). The debugger hooks the
//      dispatch loop and watches that line CHANGE — that is what "stepping one
//      line" means at the machine level.
//
//   2. Printing a local by NAME needs DEBUG INFORMATION: compiled code refers to
//      locals only by stack slot — the names are gone. The compiler now emits a
//      per-function table mapping name -> slot over a bytecode range
//      (LocalDebug in chunk.h), which is exactly what DWARF does for native
//      binaries, in miniature.
#ifndef CNANO_DEBUGGER_H
#define CNANO_DEBUGGER_H

#include "vm.h" // CallFrame

// Global debugger state. `active` gates a single predictable branch in the VM's
// dispatch loop (the same pattern as the --stats counters), so a non-debug run
// pays almost nothing.
typedef struct {
  bool active;
  bool stepping;      // pause at the next new source line...
  int stepFrameDepth; // ...but only at this call depth or shallower ("next");
                      // INT_MAX for "step" (any depth — steps into calls)
  int stepFromLine;   // the line `next` was issued from: returning to it in the
                      // SAME frame (after a call within the line) must not
                      // re-pause — it is still the same statement. -1 for "step"
  int currentLine;    // the line of the previous pause/check, to detect changes
  int breakpoints[64];
  int breakpointCount;
  char *source;       // the root file's text, for showing the paused line
} Debugger;

extern Debugger debugger;

// Switch the debugger on for the program at `path` (reads the file's text so
// paused lines can be shown). Call before interpretFile().
void debuggerStart(const char *path);

// Free the debugger's resources (the source text).
void debuggerEnd(void);

// The per-instruction hook, called by the VM's dispatch loop while `active`.
// Detects source-line changes, decides whether to pause (stepping or a
// breakpoint), and runs the interactive command prompt while paused.
void debuggerHook(CallFrame *frame);

#endif // CNANO_DEBUGGER_H
