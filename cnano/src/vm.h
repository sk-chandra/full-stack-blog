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

// A fixed-size operand stack. 256 slots is plenty for arithmetic expressions;
// real VMs grow the stack dynamically. A fixed cap keeps the code simple and
// makes stack-overflow handling explicit.
#define STACK_MAX 256

typedef struct {
  Chunk *chunk;        // the bytecode being executed
  uint8_t *ip;         // instruction pointer: the NEXT byte to read
  Value stack[STACK_MAX];
  Value *stackTop;     // points just PAST the last pushed value
  Value lastResult;    // value produced by the most recent OP_RETURN
} VM;

// The result of a run, so the CLI can pick the right process exit code.
typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR,
} InterpretResult;

void initVM(void);
void freeVM(void);

// Compile + run `source`. If `trace` is true, dump the chunk and print the
// stack at every step — the best way to learn how the VM "thinks".
// On success, the final value is written to *out (may be NULL to ignore).
InterpretResult interpret(const char *source, bool trace, Value *out);

#endif // CNANO_VM_H
