#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codegen_c.h"
#include "compiler.h"
#include "debug.h"
#include "object.h"
#include "optimize.h"
#include "parser.h"
#include "typecheck.h"
#include "vm.h"

// The one global VM instance (declared `extern` in vm.h).
VM vm;

// Reset the stack AND the call frames. No need to clear memory — values below
// stackTop and frames below frameCount are simply considered "not there".
static void resetStack(void) {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
}

void initVM(void) {
  resetStack();
  vm.objects = NULL;
  vm.openUpvalues = NULL;
  initTable(&vm.globals);
  initTable(&vm.strings);
}

void freeVM(void) {
  // Free the bookkeeping tables, then every heap object. The intern table holds
  // borrowed pointers to the same ObjStrings that freeObjects() frees, so we
  // must free the TABLE storage first, then the objects — never the reverse.
  freeTable(&vm.globals);
  freeTable(&vm.strings);
  freeObjects();
}

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

  // Print a STACK TRACE: walk the active frames from innermost to outermost,
  // naming each function and the line it was executing. This is the payoff of
  // keeping per-frame ip's — we can reconstruct exactly how we got here.
  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame *frame = &vm.frames[i];
    ObjFunction *function = frame->closure->function;
    // frame->ip points at the NEXT instruction, so -1 gets the current one.
    size_t instruction = frame->ip - function->chunk.code - 1;
    int line = function->chunk.lines[instruction];
    fprintf(stderr, "[line %d] in ", line);
    if (function->name == NULL)
      fprintf(stderr, "script\n");
    else
      fprintf(stderr, "%s()\n", function->name->chars);
  }

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

// Concatenate the two strings on top of the stack into a new one. We build a
// fresh char buffer, then hand it to copyString — which, thanks to interning,
// returns the existing object if this exact text already exists. We peek (not
// pop) the operands until the new string is safely created; if a GC existed,
// keeping them reachable would matter here.
static void concatenate(void) {
  ObjString *b = AS_STRING(peek(0));
  ObjString *a = AS_STRING(peek(1));
  int length = a->length + b->length;
  char *chars = malloc(length + 1);
  if (chars == NULL) {
    fprintf(stderr, "cnano: out of memory concatenating strings\n");
    exit(70);
  }
  memcpy(chars, a->chars, a->length);
  memcpy(chars + a->length, b->chars, b->length);
  chars[length] = '\0';
  ObjString *result = copyString(chars, length);
  free(chars); // copyString copies; this temporary buffer is no longer needed
  pop();       // b
  pop();       // a
  push(OBJ_VAL(result));
}

// --- the calling convention, runtime side ----------------------------------
//
// Begin a call to `function` with `argCount` arguments already pushed. We set up
// a new CallFrame whose window (`slots`) starts at the callee on the stack, so:
//   stack:  [.. callee arg0 arg1 .. arg(N-1)]
//                  ^slots[0]  slots[1] ...
// Slot 0 is the callee itself (the compiler reserved it); the arguments are the
// next slots, which is exactly where the function's parameters live. Returns
// false on an error (arity mismatch or call-stack overflow).
static bool call(ObjClosure *closure, int argCount) {
  ObjFunction *function = closure->function;
  if (argCount != function->arity) {
    runtimeError("%s() expects %d arguments but got %d",
                 function->name ? function->name->chars : "fn",
                 function->arity, argCount);
    return false;
  }
  if (vm.frameCount == FRAMES_MAX) {
    runtimeError("stack overflow (call depth exceeded %d)", FRAMES_MAX);
    return false;
  }
  CallFrame *frame = &vm.frames[vm.frameCount++];
  frame->closure = closure;
  frame->ip = function->chunk.code;          // start at the function's first byte
  frame->slots = vm.stackTop - argCount - 1; // window includes callee + args
  return true;
}

// Dispatch a call on whatever value is being called. Only closures are callable
// (every function becomes a closure); calling anything else is a clean error.
static bool callValue(Value callee, int argCount) {
  if (IS_CLOSURE(callee))
    return call(AS_CLOSURE(callee), argCount);
  runtimeError("can only call functions");
  return false;
}

// Find-or-create the upvalue that captures the stack slot at `local`. The open
// upvalue list is kept sorted by slot address (highest first). If an upvalue
// already points at this exact slot we REUSE it — so two closures capturing the
// same variable share one upvalue and therefore see each other's writes. That
// sharing is what makes a counter-closure pair work.
static ObjUpvalue *captureUpvalue(Value *local) {
  ObjUpvalue *prev = NULL;
  ObjUpvalue *upvalue = vm.openUpvalues;
  while (upvalue != NULL && upvalue->location > local) {
    prev = upvalue;
    upvalue = upvalue->next;
  }
  if (upvalue != NULL && upvalue->location == local)
    return upvalue; // already captured this slot

  ObjUpvalue *created = newUpvalue(local);
  created->next = upvalue;
  if (prev == NULL)
    vm.openUpvalues = created;
  else
    prev->next = created;
  return created;
}

// Close every open upvalue at or above `last` (a stack address). "Closing" copies
// the live value out of the dying stack slot into the upvalue's own `closed`
// field and repoints `location` there — so the variable survives on the heap
// after its stack slot is gone. Called when locals leave scope and on return.
static void closeUpvalues(Value *last) {
  while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
    ObjUpvalue *upvalue = vm.openUpvalues;
    upvalue->closed = *upvalue->location; // copy value off the stack
    upvalue->location = &upvalue->closed; // redirect to the heap home
    vm.openUpvalues = upvalue->next;
  }
}

// The fetch-decode-execute loop — the core of the whole project.
static InterpretResult run(bool trace) {
  // The currently executing frame. We cache it in a local for speed and re-cache
  // it whenever we call into or return from a function (the only times it
  // changes). All reads of bytecode and locals now go through `frame`.
  CallFrame *frame = &vm.frames[vm.frameCount - 1];
// These macros make the loop read like an instruction reference. READ_BYTE
// fetches the next byte and advances ip; READ_CONSTANT uses that byte as a pool
// index. BINARY_OP factors out the identical pop/pop/push shape of +,-,*,/.
#define READ_BYTE() (*frame->ip++)
// Read a 2-byte big-endian operand (used by jumps) and advance ip past it.
#define READ_SHORT()                                                           \
  (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
// Constants come from the current closure's function's chunk, via the frame.
#define READ_CONSTANT()                                                        \
  (frame->closure->function->chunk.constants.values[READ_BYTE()])
// Read a constant and interpret it as a string — used for variable names, which
// the compiler always stores as ObjString constants.
#define READ_STRING() (AS_STRING(READ_CONSTANT()))
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
      Chunk *ch = &frame->closure->function->chunk;
      disassembleInstruction(ch, (int)(frame->ip - ch->code));
    }

    uint8_t instruction = READ_BYTE();
    switch (instruction) {
    case OP_CONSTANT: {
      Value constant = READ_CONSTANT();
      push(constant);
      break;
    }
    case OP_CONSTANT_LONG: {
      // Reassemble the 3-byte index, then index the constant pool.
      int index = (READ_BYTE() << 16);
      index |= (READ_BYTE() << 8);
      index |= READ_BYTE();
      push(frame->closure->function->chunk.constants.values[index]);
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
      // `+` is OVERLOADED: integers add, strings concatenate. The VM inspects
      // the operand types at runtime and dispatches — the essence of operator
      // overloading in a dynamically typed language. Anything else is an error.
      if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
        concatenate();
      } else if (IS_INT(peek(0)) && IS_INT(peek(1))) {
        int64_t b = AS_INT(pop());
        int64_t a = AS_INT(pop());
        push(INT_VAL(a + b));
      } else {
        runtimeError("operands to '+' must be two integers or two strings");
        return INTERPRET_RUNTIME_ERROR;
      }
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
    case OP_DEFINE_GLOBAL: {
      // The operand indexes the name in the constant pool. We read the value
      // from the stack, store it, then pop. Defining over an existing global is
      // allowed (it just overwrites) — a deliberate, lenient choice.
      ObjString *name = READ_STRING();
      tableSet(&vm.globals, name, peek(0));
      pop();
      break;
    }
    case OP_GET_GLOBAL: {
      ObjString *name = READ_STRING();
      Value value;
      if (!tableGet(&vm.globals, name, &value)) {
        // Reading a name that was never defined is a runtime error — the safety
        // guarantee that makes variables usable. (A typo'd name fails loudly.)
        runtimeError("undefined variable '%s'", name->chars);
        return INTERPRET_RUNTIME_ERROR;
      }
      push(value);
      break;
    }
    case OP_SET_GLOBAL: {
      ObjString *name = READ_STRING();
      // tableSet returns true when it ADDED a new key. Assignment must only
      // update an EXISTING variable, so if it was new we delete it back out and
      // error. Unlike DEFINE, assignment does NOT pop: `a = 1` is an expression
      // whose value (1) stays on the stack for the surrounding context.
      if (tableSet(&vm.globals, name, peek(0))) {
        tableDelete(&vm.globals, name);
        runtimeError("undefined variable '%s'", name->chars);
        return INTERPRET_RUNTIME_ERROR;
      }
      break;
    }
    case OP_GET_LOCAL: {
      // A local lives at a fixed slot WITHIN THE CURRENT FRAME's window. The slot
      // index is relative to frame->slots, not the absolute stack base — that is
      // what lets the same bytecode address a different physical location on each
      // (re)entry, which is precisely how recursion gets independent locals.
      uint8_t slot = READ_BYTE();
      push(frame->slots[slot]);
      break;
    }
    case OP_SET_LOCAL: {
      // Store the top value INTO the local's slot (frame-relative). peek (not
      // pop): assignment is an expression, so its value stays on top.
      uint8_t slot = READ_BYTE();
      frame->slots[slot] = peek(0);
      break;
    }
    case OP_GET_UPVALUE: {
      // Read through the upvalue's indirection: `location` points either at a
      // live stack slot (open) or at the upvalue's own heap cell (closed). Either
      // way this is the captured variable's current value.
      uint8_t slot = READ_BYTE();
      push(*frame->closure->upvalues[slot]->location);
      break;
    }
    case OP_SET_UPVALUE: {
      // Write through the same indirection — so a closure can MUTATE a captured
      // variable, and any other closure sharing that upvalue sees the change.
      uint8_t slot = READ_BYTE();
      *frame->closure->upvalues[slot]->location = peek(0);
      break;
    }
    case OP_JUMP: {
      // Unconditional forward jump: always skip `offset` bytes (within the
      // current function's ip).
      uint16_t offset = READ_SHORT();
      frame->ip += offset;
      break;
    }
    case OP_JUMP_IF_FALSE: {
      // Conditional jump. We PEEK (not pop) the condition: the compiler decides
      // when to pop it (the symmetric POPs in if/while), which keeps this opcode
      // reusable for short-circuit and/or where the value is also the result.
      uint16_t offset = READ_SHORT();
      if (isFalsey(peek(0)))
        frame->ip += offset;
      break;
    }
    case OP_LOOP: {
      // Unconditional backward jump: the engine of every loop.
      uint16_t offset = READ_SHORT();
      frame->ip -= offset;
      break;
    }
    case OP_CALL: {
      // The callee sits `argCount` slots below the top. Dispatch the call; on
      // success a new frame was pushed, so re-cache `frame` to the new innermost
      // one and the loop continues executing the callee's bytecode.
      int argCount = READ_BYTE();
      Value callee = peek(argCount);
      if (!callValue(callee, argCount))
        return INTERPRET_RUNTIME_ERROR;
      frame = &vm.frames[vm.frameCount - 1];
      break;
    }
    case OP_CLOSURE: {
      // Build a closure from a function constant, then capture each upvalue as
      // described by the trailing operand pairs. For a LOCAL capture we grab the
      // upvalue for a slot in the CURRENT frame; for a non-local we copy the
      // current closure's own upvalue (the chaining set up by the compiler). This
      // runs at the point the `fn` is evaluated, snapshotting the environment.
      ObjFunction *function = AS_FUNCTION(READ_CONSTANT());
      ObjClosure *closure = newClosure(function);
      push(OBJ_VAL(closure));
      for (int i = 0; i < closure->upvalueCount; i++) {
        uint8_t isLocal = READ_BYTE();
        uint8_t index = READ_BYTE();
        if (isLocal)
          closure->upvalues[i] = captureUpvalue(frame->slots + index);
        else
          closure->upvalues[i] = frame->closure->upvalues[index];
      }
      break;
    }
    case OP_CLOSE_UPVALUE:
      // A captured local is leaving scope: lift it to the heap, then pop it.
      closeUpvalues(vm.stackTop - 1);
      pop();
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
    case OP_RETURN: {
      // Return from the current function. The return value is on top. Before
      // tearing down the frame we CLOSE any upvalues that captured this frame's
      // slots — a closure created inside this call (and returned, or stored
      // elsewhere) must keep working after the frame is gone.
      Value result = pop();
      closeUpvalues(frame->slots);
      vm.frameCount--;
      if (vm.frameCount == 0) {
        pop(); // discard the top-level script's reserved slot 0
        return INTERPRET_OK;
      }
      vm.stackTop = frame->slots; // reclaim the callee's window
      push(result);               // hand the result to the caller
      frame = &vm.frames[vm.frameCount - 1]; // resume the caller
      break;
    }
    }
  }

#undef READ_BYTE
#undef READ_SHORT
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

  // 1b. STATIC TYPE CHECK. A separate analysis pass over the AST that catches
  // type errors before any code runs. Gradual: unannotated code is `any` and
  // passes trivially. On failure we refuse to compile or run, like a real
  // ahead-of-time compiler rejecting an ill-typed program.
  if (!typecheckProgram(&program)) {
    freeProgram(&program);
    return INTERPRET_COMPILE_ERROR;
  }

  // 1c. OPTIMISE the AST. Constant folding rewrites constant subexpressions into
  // literals (`2 + 3 * 4` -> `14`) so the VM never recomputes them. Runs after
  // type-checking (which used the original tree for accurate error lines) and
  // before compilation, which then emits bytecode for the simpler tree.
  foldConstants(&program);

  // 2. Compile the program into a top-level ObjFunction. (Its chunk, and every
  // nested function's chunk, is owned by the VM object list — freed at shutdown,
  // not here.)
  ObjFunction *function = compile(&program);
  freeProgram(&program); // trees no longer needed once bytecode exists
  if (function == NULL)
    return INTERPRET_COMPILE_ERROR;

  if (trace)
    disassembleChunk(&function->chunk, "<script>");

  // 3. Bootstrap execution: wrap the script function in a closure (the VM only
  // ever calls closures), push it, and `call` it, so the top level runs through
  // the exact same frame machinery as any function.
  ObjClosure *closure = newClosure(function);
  push(OBJ_VAL(closure));
  call(closure, 0);
  return run(trace);
}

InterpretResult compileToC(const char *source, FILE *cFile) {
  // Identical front end to interpret(): parse, type-check, fold. Reusing it means
  // the native path accepts exactly the programs the VM does (and rejects the
  // same errors), differing only in the BACKEND it feeds the AST to.
  Program program;
  if (!parse(source, &program)) {
    freeProgram(&program);
    return INTERPRET_COMPILE_ERROR;
  }
  if (!typecheckProgram(&program)) {
    freeProgram(&program);
    return INTERPRET_COMPILE_ERROR;
  }
  foldConstants(&program);

  bool gen = emitC(&program, cFile);
  freeProgram(&program);
  return gen ? INTERPRET_OK : INTERPRET_COMPILE_ERROR;
}
