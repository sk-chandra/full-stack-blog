// vm.h — the virtual machine that executes bytecode.
//
// The VM is a software CPU. It has an instruction pointer (which byte it is
// about to execute) and an operand stack (scratch space for intermediate
// values). It runs a "fetch-decode-execute" loop, the same cycle a real
// processor runs in hardware. Building this loop is where the abstract idea of
// "a program" becomes concrete and mechanical.
#ifndef CNANO_VM_H
#define CNANO_VM_H

#include "chunk.h"
#include "object.h"
#include "table.h"

// The maximum call depth (number of nested function calls). Exceeding it is a
// "stack overflow" runtime error — the controlled version of what infinite
// recursion does to a native program.
#define FRAMES_MAX 64
// The operand stack must be big enough for all locals/temporaries across all
// active frames, so it scales with the frame limit.
#define STACK_MAX (FRAMES_MAX * 256)

// One CALL FRAME: the activation record of a single in-progress function call.
// This is the central data structure of step 6. Each frame remembers:
//   - which function is running (so we know its chunk),
//   - that function's OWN instruction pointer (so calls/returns resume correctly),
//   - and `slots`: a pointer to where this call's window begins on the shared
//     operand stack. A local at compile-time "slot i" is simply slots[i] — that
//     is how the same bytecode runs at a different stack location every call,
//     which is exactly what makes recursion work.
typedef struct {
  ObjClosure *closure; // the closure being run (its function holds the chunk)
  uint8_t *ip;
  Value *slots;
} CallFrame;

// One active `try` handler: where to jump on a throw, and the stack/frame state
// to restore when unwinding to it.
typedef struct {
  uint8_t *handlerIp; // the catch code
  Value *stackTop;    // stack depth to unwind to
  int frameCount;     // call depth to unwind to
} TryHandler;

#define TRY_MAX 64

typedef struct {
  CallFrame frames[FRAMES_MAX]; // the call stack: one frame per active call
  int frameCount;               // current call depth
  TryHandler handlers[TRY_MAX]; // active try/catch handlers (a stack)
  int handlerCount;

  Value stack[STACK_MAX];
  Value *stackTop;     // points just PAST the last pushed value
  Table globals;       // global variable store: name (ObjString*) -> Value
  Table strings;       // string intern pool, used as a set of all live strings
  ObjString *initString; // the interned name "init" (the constructor method)
  // Bumped whenever a NEW global is defined (which may rebuild the globals
  // table). The per-chunk global inline cache stores this token alongside a
  // cached entry index, and re-resolves whenever the token has moved on.
  uint32_t globalsGen;

  // --- generator suspend/resume bookkeeping ---
  // While a generator is being resumed, `resuming` points at it so OP_YIELD can
  // freeze the frame back into it; `didYield` distinguishes a suspend (yield)
  // from a completed return when run() hands control back to resumeGenerator.
  ObjGenerator *resuming;
  bool didYield;
  ObjUpvalue *openUpvalues; // open upvalues, sorted by stack slot (highest first)
  Obj *objects;        // head of the intrusive list of every heap object

  // --- garbage-collector bookkeeping ---
  bool gcEnabled;       // collector runs only while the VM is executing (see memory.h)
  size_t bytesAllocated; // running total of live bytes, maintained by reallocate
  size_t nextGC;         // collect when bytesAllocated exceeds this threshold
  Obj **grayStack;       // the mark phase's grey worklist (managed outside the GC)
  int grayCount;
  int grayCapacity;

  // --- optional profiling (`--stats`) ---
  // When enabled, the VM tallies instructions (per opcode), heap allocations and
  // GC cycles, then prints a summary — so you MEASURE before you optimise.
  bool collectStats;
  size_t opCounts[256]; // executions per opcode
  size_t instrCount;    // total instructions executed
  size_t allocCount;    // distinct heap allocations
  size_t allocBytes;    // total bytes handed out by the allocator
  size_t gcCount;       // garbage-collection cycles
  // --- garbage-collector instrumentation ---
  size_t gcReclaimed;   // total bytes reclaimed across all collections
  size_t gcMicros;      // total time spent collecting (microseconds)
  size_t gcPeakLive;    // high-water mark of live bytes
  bool gcTrace;         // log every collection (set by the CNANO_GC_TRACE env var)
} VM;

// Print the profiling summary gathered during a `collectStats` run.
void printVmStats(void);

// The VM is a single global instance. object.c reaches in to register new
// objects (vm.objects) and intern strings (vm.strings), so the struct is exposed
// here rather than hidden in vm.c.
extern VM vm;

// The result of a run, so the CLI can pick the right process exit code.
typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR,
} InterpretResult;

void initVM(void);
void freeVM(void);

// Report a runtime error (printf-style) and print a stack trace. Exposed so the
// builtins (builtins.c) can fail cleanly — e.g. a bad argument or a missing
// method — using the same machinery as the VM's own checks.
void runtimeError(const char *format, ...);

// Stack access for builtins that need to root temporaries across a re-entrant
// call (e.g. an accumulator that must survive a GC triggered inside a callback).
void push(Value value);
Value pop(void);

// Call a cnano callable (closure) FROM C, used by higher-order builtins like
// array.map(fn). Pushes `callee` and the `argCount` arguments, runs the VM until
// that call returns, and writes the result through `*result`. Returns false if
// the call raised a runtime error. This is the VM being re-entered from within a
// native — the mechanism that lets C and cnano code call each other freely.
bool callFromVM(Value callee, Value *args, int argCount, Value *result);

// Resume a suspended generator: thaw its saved frame, run to the next `yield`
// (or its final return), then re-freeze. Sets *result to the yielded/returned
// value and *finished to whether the generator has now completed. Returns false
// on a runtime error. Used by the `.next()` builtin method.
bool resumeGenerator(ObjGenerator *gen, Value *result, bool *finished);

// Compile + run `source` (a sequence of statements). If `trace` is true, dump
// every function's chunk and print the stack at each step — the best way to learn
// how the VM "thinks". Programs produce output via `print`.
InterpretResult interpret(const char *source, bool trace);

// Like interpret(), but reads a FILE and resolves its `import "…"` statements
// relative to the file's own directory. The CLI uses this for `cnano FILE.cn`.
InterpretResult interpretFile(const char *path, bool trace);

// NATIVE backend. Compile `source` (the typed first-order subset) to C and write
// it to `cFile`. Runs the same front end (parse, type-check, fold) as the VM, so
// errors are caught identically. Returns INTERPRET_OK on success. `emitOnly` has
// no effect here; the caller decides whether to invoke `cc` on the result.
InterpretResult compileToC(const char *source, FILE *cFile);

// Like compileToC(), but reads a FILE and resolves its imports relative to the
// file's directory (used by `--emit-c` and `--native`).
InterpretResult compileFileToC(const char *path, FILE *cFile);

// Compile FILE and print the control-flow graph of every function (`--cfg`).
InterpretResult dumpCFGFile(const char *path);

// Lower FILE's straight-line code to three-address IR and print it, before and
// after the IR optimiser (`--ir`).
InterpretResult dumpIRFile(const char *path);

// Type-check FILE and print the inferred static type of every top-level binding
// (`--types`) — return-type inference and let-inference made visible.
InterpretResult dumpTypesFile(const char *path);

// Compile FILE's straight-line integer code through the IR to x86-64 assembly,
// written to stdout (`--asm`). The real machine-code back end.
InterpretResult dumpAsmFile(const char *path);

// Compile FILE's integer subset directly to a native ELF executable at
// `outPath` (`--elf FILE -o OUT`) — no assembler, no linker, no libc.
InterpretResult compileElfFile(const char *path, const char *outPath);

#endif // CNANO_VM_H
