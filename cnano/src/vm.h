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
  ObjFunction *function;
  uint8_t *ip;
  Value *slots;
} CallFrame;

typedef struct {
  CallFrame frames[FRAMES_MAX]; // the call stack: one frame per active call
  int frameCount;               // current call depth

  Value stack[STACK_MAX];
  Value *stackTop;     // points just PAST the last pushed value
  Table globals;       // global variable store: name (ObjString*) -> Value
  Table strings;       // string intern pool, used as a set of all live strings
  Obj *objects;        // head of the intrusive list of every heap object
} VM;

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

// Compile + run `source` (a sequence of statements). If `trace` is true, dump
// every function's chunk and print the stack at each step — the best way to learn
// how the VM "thinks". Programs produce output via `print`.
InterpretResult interpret(const char *source, bool trace);

#endif // CNANO_VM_H
