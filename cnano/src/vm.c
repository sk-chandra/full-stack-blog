#include <stdarg.h>
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

// Inspect a value without popping it. `distance` 0 is the top, 1 is below it.
// Type checks need to look at operands before deciding whether it is even safe
// to pop and use them.
static Value peek(int distance) { return vm.stackTop[-1 - distance]; }

// Now takes a printf-style format so callers can include the offending types in
// the message. varargs (stdarg.h) is the idiomatic C way to do this.
static void runtimeError(const char *format, ...) {
  va_list args;
  va_start(args, format);
  fprintf(stderr, "Runtime error: ");
  vfprintf(stderr, format, args);
  va_end(args);
  fprintf(stderr, "\n");

  // Recover the source line from the instruction that just executed. vm.ip has
  // already advanced past it, hence the -1.
  int instruction = (int)(vm.ip - vm.chunk->code) - 1;
  int line = vm.chunk->lines[instruction];
  fprintf(stderr, "[line %d] in script\n", line);
  resetStack();
}

// cnano's truthiness rule, used by OP_NOT (and later by `if`/`while`). This is a
// genuine language-design decision with no universally "right" answer:
//   - nil is falsey
//   - false is falsey; true is truthy
//   - every integer (including 0!) is truthy
// Choosing that 0 is truthy is deliberate — it keeps booleans and integers as
// distinct ideas. Languages like C and Python treat 0 as falsey; Ruby and Lua
// treat only nil/false as falsey (the rule we adopt here).
static bool isFalsey(Value value) {
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

// The fetch-decode-execute loop — the core of the whole project.
static InterpretResult run(bool trace) {
// These macros make the loop read like an instruction reference. READ_BYTE
// fetches the next byte and advances ip; READ_CONSTANT uses that byte as a pool
// index. BINARY_OP factors out the identical pop/pop/push shape of +,-,*,/.
#define READ_BYTE() (*vm.ip++)
#define READ_CONSTANT() (vm.chunk->constants.values[READ_BYTE()])
// BINARY_OP now does three jobs the bare-int version didn't have to: (1) verify
// both operands are integers BEFORE touching them, erroring cleanly if not; (2)
// unwrap the tagged Values to raw int64; (3) re-wrap the result with the given
// constructor macro (INT_VAL for arithmetic, BOOL_VAL for comparisons). Passing
// the wrapper as `valueType` lets +,-,*,<,> all share this one shape.
#define BINARY_OP(valueType, op)                                               \
  do {                                                                         \
    if (!IS_INT(peek(0)) || !IS_INT(peek(1))) {                                \
      runtimeError("operands must be integers");                              \
      return INTERPRET_RUNTIME_ERROR;                                          \
    }                                                                          \
    int64_t b = AS_INT(pop());                                                 \
    int64_t a = AS_INT(pop());                                                 \
    push(valueType(a op b));                                                   \
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
    case OP_NIL:
      push(NIL_VAL);
      break;
    case OP_TRUE:
      push(BOOL_VAL(true));
      break;
    case OP_FALSE:
      push(BOOL_VAL(false));
      break;
    case OP_NEGATE:
      // Unary minus is integers-only, so it gets its own type check (the
      // BINARY_OP macro doesn't apply to a one-operand instruction).
      if (!IS_INT(peek(0))) {
        runtimeError("operand of '-' must be an integer");
        return INTERPRET_RUNTIME_ERROR;
      }
      push(INT_VAL(-AS_INT(pop())));
      break;
    case OP_NOT:
      // `!` works on ANY value via the truthiness rule, so no type check — it
      // always yields a boolean. This asymmetry with OP_NEGATE is intentional
      // and worth noticing.
      push(BOOL_VAL(isFalsey(pop())));
      break;
    case OP_ADD:
      BINARY_OP(INT_VAL, +);
      break;
    case OP_SUB:
      BINARY_OP(INT_VAL, -);
      break;
    case OP_MUL:
      BINARY_OP(INT_VAL, *);
      break;
    case OP_DIV: {
      // Division needs the integer type check AND a zero-divisor check. Order
      // matters: validate types first (so the error message is right), then
      // guard against divide-by-zero, which is undefined behaviour in C and
      // would otherwise crash the whole interpreter.
      if (!IS_INT(peek(0)) || !IS_INT(peek(1))) {
        runtimeError("operands must be integers");
        return INTERPRET_RUNTIME_ERROR;
      }
      if (AS_INT(peek(0)) == 0) {
        runtimeError("division by zero");
        return INTERPRET_RUNTIME_ERROR;
      }
      int64_t b = AS_INT(pop());
      int64_t a = AS_INT(pop());
      push(INT_VAL(a / b));
      break;
    }
    case OP_EQUAL: {
      // Equality is defined for ALL types (via valuesEqual), so unlike the
      // ordering comparisons it needs no integer check.
      Value b = pop();
      Value a = pop();
      push(BOOL_VAL(valuesEqual(a, b)));
      break;
    }
    case OP_LESS:
      BINARY_OP(BOOL_VAL, <);
      break;
    case OP_GREATER:
      BINARY_OP(BOOL_VAL, >);
      break;
    case OP_PRINT:
      // The only way a cnano program produces output. It pops its operand, so
      // like every statement-level op it is stack-neutral overall.
      printValue(pop());
      printf("\n");
      break;
    case OP_POP:
      pop(); // discard the result of an expression statement
      break;
    case OP_RETURN:
      // End of program. Nothing to return — output already happened via print.
      return INTERPRET_OK;
    }
  }

#undef READ_BYTE
#undef READ_CONSTANT
#undef BINARY_OP
}

InterpretResult interpret(const char *source, bool trace) {
  // 1. Parse source text into a Program (a list of statement trees).
  Program program;
  bool ok = parse(source, &program);
  if (!ok) {
    freeProgram(&program); // may hold partially-built statements
    return INTERPRET_COMPILE_ERROR;
  }

  // 2. Compile the program into a chunk of bytecode.
  Chunk chunk;
  initChunk(&chunk);
  compile(&program, &chunk);
  freeProgram(&program); // trees no longer needed once bytecode exists

  if (trace)
    disassembleChunk(&chunk, "compiled bytecode");

  // 3. Point the VM at the chunk and run it.
  vm.chunk = &chunk;
  vm.ip = vm.chunk->code;
  InterpretResult result = run(trace);

  freeChunk(&chunk);
  return result;
}
