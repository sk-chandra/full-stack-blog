#include <stdio.h>

#include "compiler.h"
#include "debug.h"
#include "parser.h"
#include "vm.h"

static VM vm;

// Reset the stack by pointing the top back at the base. No need to clear the
// memory — values below stackTop are simply considered "not there".
static void resetStack(void) { vm.stackTop = vm.stack; }

void initVM(void) { resetStack(); }

void freeVM(void) { /* nothing heap-allocated in the VM itself yet */ }

static void push(Value value) {
  // (A production VM checks for overflow here; our expressions can't exceed
  // STACK_MAX, but it's worth knowing this is where that guard belongs.)
  *vm.stackTop = value;
  vm.stackTop++;
}

static Value pop(void) {
  vm.stackTop--;
  return *vm.stackTop;
}

static void runtimeError(const char *message) {
  // Recover the source line from the instruction that just executed. vm.ip has
  // already advanced past it, hence the -1.
  int instruction = (int)(vm.ip - vm.chunk->code) - 1;
  int line = vm.chunk->lines[instruction];
  fprintf(stderr, "[line %d] Runtime error: %s\n", line, message);
  resetStack();
}

// The fetch-decode-execute loop — the core of the whole project.
static InterpretResult run(bool trace) {
// These macros make the loop read like an instruction reference. READ_BYTE
// fetches the next byte and advances ip; READ_CONSTANT uses that byte as a pool
// index. BINARY_OP factors out the identical pop/pop/push shape of +,-,*,/.
#define READ_BYTE() (*vm.ip++)
#define READ_CONSTANT() (vm.chunk->constants.values[READ_BYTE()])
#define BINARY_OP(op)                                                          \
  do {                                                                         \
    Value b = pop();                                                           \
    Value a = pop();                                                           \
    push(a op b);                                                              \
  } while (false)

  for (;;) {
    if (trace) {
      // Show the current stack contents, then the instruction about to run.
      printf("          ");
      for (Value *slot = vm.stack; slot < vm.stackTop; slot++) {
        printf("[ ");
        printValue(*slot);
        printf(" ]");
      }
      printf("\n");
      disassembleInstruction(vm.chunk, (int)(vm.ip - vm.chunk->code));
    }

    uint8_t instruction = READ_BYTE();
    switch (instruction) {
    case OP_CONSTANT: {
      Value constant = READ_CONSTANT();
      push(constant);
      break;
    }
    case OP_NEGATE:
      push(-pop());
      break;
    case OP_ADD:
      BINARY_OP(+);
      break;
    case OP_SUB:
      BINARY_OP(-);
      break;
    case OP_MUL:
      BINARY_OP(*);
      break;
    case OP_DIV: {
      // Division by zero is undefined behaviour in C for integers and would
      // crash the whole interpreter. A language must turn such hardware faults
      // into controlled runtime errors — this is a key robustness lesson.
      Value b = pop();
      Value a = pop();
      if (b == 0) {
        push(a); // restore a token value so the stack stays consistent
        runtimeError("division by zero");
        return INTERPRET_RUNTIME_ERROR;
      }
      push(a / b);
      break;
    }
    case OP_RETURN: {
      Value result = pop();
      if (trace) {
        printf("== result ==\n");
      }
      printValue(result);
      printf("\n");
      // Stash the result for the caller before returning.
      vm.lastResult = result;
      return INTERPRET_OK;
    }
    }
  }

#undef READ_BYTE
#undef READ_CONSTANT
#undef BINARY_OP
}

InterpretResult interpret(const char *source, bool trace, Value *out) {
  // 1. Parse source text into an AST.
  Node *tree = parse(source);
  if (tree == NULL)
    return INTERPRET_COMPILE_ERROR;

  // 2. Compile the AST into a chunk of bytecode.
  Chunk chunk;
  initChunk(&chunk);
  compile(tree, &chunk);
  freeNode(tree); // the tree is no longer needed once bytecode exists

  if (trace)
    disassembleChunk(&chunk, "compiled bytecode");

  // 3. Point the VM at the chunk and run it.
  vm.chunk = &chunk;
  vm.ip = vm.chunk->code;
  InterpretResult result = run(trace);

  if (out != NULL && result == INTERPRET_OK)
    *out = vm.lastResult;

  freeChunk(&chunk);
  return result;
}
