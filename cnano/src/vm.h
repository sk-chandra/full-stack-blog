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
#include "table.h"

// A fixed-size operand stack. 256 slots is plenty for arithmetic expressions;
// real VMs grow the stack dynamically. A fixed cap keeps the code simple and
// makes stack-overflow handling explicit.
#define STACK_MAX 256

typedef struct {
  Chunk *chunk;        // the bytecode being executed
  uint8_t *ip;         // instruction pointer: the NEXT byte to read
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
// the chunk and print the stack at every step — the best way to learn how the VM
// "thinks". Programs now produce output via `print`, so there is no return value
// to hand back.
InterpretResult interpret(const char *source, bool trace);

#endif // CNANO_VM_H
