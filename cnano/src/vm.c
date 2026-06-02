#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"
#include "codegen_c.h"
#include "codegen_x64.h"
#include "compiler.h"
#include "cfg.h"
#include "debug.h"
#include "ir.h"
#include "memory.h"
#include "module.h"
#include "object.h"
#include "optimize.h"
#include "parser.h"
#include "suggest.h" // "did you mean …?" for undefined globals
#include "type.h"
#include "typecheck.h"
#include "vm.h"

// The one global VM instance (declared `extern` in vm.h).
VM vm;

// Reset the stack AND the call frames. No need to clear memory — values below
// stackTop and frames below frameCount are simply considered "not there".
static void resetStack(void) {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.handlerCount = 0; // discard any active try handlers on (re)start / abort
}

void initVM(void) {
  resetStack();
  vm.objects = NULL;
  vm.openUpvalues = NULL;

  // GC starts DISABLED: compilation allocates objects (e.g. interned strings the
  // AST points at) that are not yet reachable from GC roots. We switch it on only
  // when execution begins (see interpret). The threshold begins at the floor.
  vm.gcEnabled = false;
  vm.bytesAllocated = 0;
  vm.nextGC = 64 * 1024;
  vm.grayStack = NULL;
  vm.grayCount = 0;
  vm.grayCapacity = 0;
  vm.globalsGen = 1; // 1, so a freshly calloc'd cache slot (gen 0) never matches
  vm.resuming = NULL;
  vm.didYield = false;
  vm.gcReclaimed = 0;
  vm.gcMicros = 0;
  vm.gcPeakLive = 0;
  vm.gcTrace = getenv("CNANO_GC_TRACE") != NULL; // log each collection if set

  initTable(&vm.globals);
  initTable(&vm.strings);

  // Intern the constructor method name once so construction can look it up by
  // pointer without allocating. (Set NULL first: copyString interns into
  // vm.strings, and a GC there must not read an uninitialised field — though GC
  // is off here, this keeps the invariant honest.)
  vm.initString = NULL;
  vm.initString = copyString("init", 4);

  // Register the built-in functions (clock, str, ...) as globals. They allocate
  // objects, which is fine: this runs before any program, with GC still off.
  defineBuiltins();
}

void freeVM(void) {
  // Free the bookkeeping tables, then every heap object. The intern table holds
  // borrowed pointers to the same ObjStrings that freeObjects() frees, so we
  // must free the TABLE storage first, then the objects — never the reverse.
  freeTable(&vm.globals);
  freeTable(&vm.strings);
  freeObjects();
  free(vm.grayStack); // the grey worklist is plain malloc memory, freed by hand
}

void push(Value value) {
  // (A production VM checks for overflow here; our expressions can't exceed
  // STACK_MAX, but it's worth knowing this is where that guard belongs.)
  *vm.stackTop = value;
  vm.stackTop++;
}

Value pop(void) {
  vm.stackTop--;
  return *vm.stackTop;
}

// Inspect a value without popping it. `distance` 0 is the top, 1 is below it.
// Type checks need to look at operands before deciding whether it is even safe
// to pop and use them.
static Value peek(int distance) { return vm.stackTop[-1 - distance]; }

// Now takes a printf-style format so callers can include the offending types in
// the message. varargs (stdarg.h) is the idiomatic C way to do this.
void runtimeError(const char *format, ...) {
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
    char loc[128];
    moduleFormatLine(line, loc, sizeof(loc)); // "line 5" or "name.cn:5"
    fprintf(stderr, "[%s] in ", loc);
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
  if (function->isGenerator) {
    // Calling a generator function does NOT run it: capture [callee, args] as the
    // generator's initial frozen window and leave a generator object in their
    // place (no frame pushed, like a native). The body runs only on .next().
    Value *base = vm.stackTop - argCount - 1; // the callee (becomes slot 0)
    ObjGenerator *gen = newGenerator(closure); // may GC; the window is rooted
    int count = argCount + 1;
    gen->saved = malloc(sizeof(Value) * count); // not GC-managed (freed in freeObject)
    if (gen->saved == NULL) {
      runtimeError("out of memory");
      return false;
    }
    memcpy(gen->saved, base, sizeof(Value) * count);
    gen->savedCount = count;
    gen->ip = function->chunk.code; // first .next() starts at the top
    vm.stackTop = base;             // pop callee + args ...
    push(OBJ_VAL(gen));             // ... and hand back the generator
    return true;                    // no frame pushed
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
  if (IS_STRUCT(callee)) {
    // Calling a struct CONSTRUCTS an instance.
    ObjStruct *s = AS_STRUCT(callee);
    Value initFn;
    if (tableGet(&s->methods, vm.initString, &initFn)) {
      // Custom constructor: allocate the instance, drop it into the callee slot
      // so it becomes the initialiser's slot 0 (`self`), then run init — which
      // is compiled to return `self`, so the call expression yields the instance.
      ObjInstance *instance = newInstance(s);
      vm.stackTop[-argCount - 1] = OBJ_VAL(instance);
      return call(AS_CLOSURE(initFn), argCount); // arity checked inside call()
    }
    // No init: positional field fill. The instance is allocated while the args
    // are still on the stack (rooted), and tableSet never collects, so it is
    // safe before we push it.
    if (argCount != s->fieldCount) {
      runtimeError("%s expects %d field%s but got %d", s->name->chars,
                   s->fieldCount, s->fieldCount == 1 ? "" : "s", argCount);
      return false;
    }
    ObjInstance *instance = newInstance(s);
    for (int i = 0; i < argCount; i++)
      tableSet(&instance->fields, s->fieldNames[i], vm.stackTop[-argCount + i]);
    vm.stackTop -= argCount + 1; // pop the arguments and the struct
    push(OBJ_VAL(instance));
    return true;
  }
  if (IS_NATIVE(callee)) {
    // A native builtin runs immediately, with no call frame: its arguments are
    // the top `argCount` stack slots. We check arity here (the uniform place),
    // call the C function, then replace callee+args with its single result.
    ObjNative *native = AS_NATIVE(callee);
    if (native->arity != argCount) {
      runtimeError("%s() expects %d arguments but got %d", native->name,
                   native->arity, argCount);
      return false;
    }
    Value result;
    if (!native->function(argCount, vm.stackTop - argCount, &result))
      return false; // the native already reported the error
    vm.stackTop -= argCount + 1; // pop the arguments and the callee
    push(result);
    return true;
  }
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

// The defined global name closest to `name`, for a "did you mean …?" hint on an
// undefined-variable error, or NULL if nothing is close. Internal "$"-prefixed
// names (like the for-in iterator) are skipped — they are never what the user
// meant to type.
static const char *suggestGlobal(const char *name) {
  const char *cands[256];
  int nc = 0;
  for (int i = 0; i < vm.globals.capacity && nc < 256; i++) {
    ObjString *key = vm.globals.entries[i].key;
    if (key != NULL && key->chars[0] != '$')
      cands[nc++] = key->chars;
  }
  return closestName(name, cands, nc);
}

// Opcode names for the --stats histogram. Designated initialisers key by the enum
// value, so this stays correct even if the opcode order changes.
static const char *OP_NAMES[256] = {
    [OP_CONSTANT] = "CONSTANT",       [OP_CONSTANT_LONG] = "CONSTANT_LONG",
    [OP_NIL] = "NIL",                 [OP_TRUE] = "TRUE",
    [OP_FALSE] = "FALSE",             [OP_NEGATE] = "NEGATE",
    [OP_NOT] = "NOT",                 [OP_ADD] = "ADD",
    [OP_SUB] = "SUB",                 [OP_MUL] = "MUL",
    [OP_DIV] = "DIV",                 [OP_MOD] = "MOD",
    [OP_BITAND] = "BITAND",           [OP_BITOR] = "BITOR",
    [OP_BITXOR] = "BITXOR",           [OP_SHL] = "SHL",
    [OP_SHR] = "SHR",                 [OP_BITNOT] = "BITNOT",
    [OP_EQUAL] = "EQUAL",             [OP_LESS] = "LESS",
    [OP_GREATER] = "GREATER",         [OP_DEFINE_GLOBAL] = "DEFINE_GLOBAL",
    [OP_GET_GLOBAL] = "GET_GLOBAL",   [OP_SET_GLOBAL] = "SET_GLOBAL",
    [OP_GET_LOCAL] = "GET_LOCAL",     [OP_SET_LOCAL] = "SET_LOCAL",
    [OP_GET_UPVALUE] = "GET_UPVALUE", [OP_SET_UPVALUE] = "SET_UPVALUE",
    [OP_JUMP] = "JUMP",               [OP_JUMP_IF_FALSE] = "JUMP_IF_FALSE",
    [OP_LOOP] = "LOOP",               [OP_PRINT] = "PRINT",
    [OP_POP] = "POP",                 [OP_CALL] = "CALL",
    [OP_TAIL_CALL] = "TAIL_CALL",
    [OP_INVOKE] = "INVOKE",           [OP_BUILD_ARRAY] = "BUILD_ARRAY",
    [OP_BUILD_MAP] = "BUILD_MAP",     [OP_INDEX_GET] = "INDEX_GET",
    [OP_INDEX_SET] = "INDEX_SET",     [OP_GET_FIELD] = "GET_FIELD",
    [OP_SET_FIELD] = "SET_FIELD",     [OP_METHOD] = "METHOD",
    [OP_BEGIN_TRY] = "BEGIN_TRY",     [OP_END_TRY] = "END_TRY",
    [OP_THROW] = "THROW",             [OP_YIELD] = "YIELD",
    [OP_IS_KIND] = "IS_KIND",
    [OP_IS_STRUCT] = "IS_STRUCT",     [OP_CLOSURE] = "CLOSURE",
    [OP_CLOSE_UPVALUE] = "CLOSE_UPVALUE", [OP_RETURN] = "RETURN",
};

void printVmStats(void) {
  fflush(stdout); // let the program's own output land before the summary
  fprintf(stderr, "\n=== cnano --stats ===\n");
  fprintf(stderr, "instructions executed : %zu\n", vm.instrCount);
  fprintf(stderr, "heap allocations      : %zu (%zu bytes)\n", vm.allocCount,
          vm.allocBytes);
  fprintf(stderr, "GC cycles             : %zu (reclaimed %zu bytes, %zu us total)\n",
          vm.gcCount, vm.gcReclaimed, vm.gcMicros);
  fprintf(stderr, "peak live heap        : %zu bytes\n", vm.gcPeakLive);

  // Opcode histogram, most-executed first (a simple selection sort over the 256
  // slots — we only print the non-zero ones).
  fprintf(stderr, "\nopcode histogram (executed, %% of total):\n");
  bool printed[256] = {false};
  for (int rank = 0; rank < 256; rank++) {
    int best = -1;
    size_t bestCount = 0;
    for (int i = 0; i < 256; i++)
      if (!printed[i] && vm.opCounts[i] > bestCount) {
        bestCount = vm.opCounts[i];
        best = i;
      }
    if (best < 0)
      break; // no more non-zero opcodes
    printed[best] = true;
    double pct = vm.instrCount ? 100.0 * (double)bestCount / (double)vm.instrCount : 0;
    const char *name = OP_NAMES[best] ? OP_NAMES[best] : "?";
    fprintf(stderr, "  %-15s %12zu  %5.1f%%\n", name, bestCount, pct);
  }
}

// The fetch-decode-execute loop — the core of the whole project. `stopFrame` is
// the call depth at which to hand control back to the caller: 0 for the top-level
// interpret() (run until the script frame returns), or the depth captured by a
// native callback (callFromVM), so re-entering the VM from C returns cleanly once
// the called function is done rather than running the whole program again.
static InterpretResult run(bool trace, int stopFrame) {
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

// Numeric arithmetic with int->float PROMOTION: two ints give an int; if either
// operand is a float, both widen to double and the result is a float.
#define NUM_ARITH(op)                                                          \
  do {                                                                         \
    if (!IS_NUM(peek(0)) || !IS_NUM(peek(1))) {                                \
      runtimeError("operands must be numbers");                                \
      return INTERPRET_RUNTIME_ERROR;                                          \
    }                                                                          \
    if (IS_INT(peek(0)) && IS_INT(peek(1))) {                                  \
      int64_t b = AS_INT(pop()), a = AS_INT(pop());                            \
      push(INT_VAL(a op b));                                                   \
    } else {                                                                   \
      /* AS_NUM evaluates its arg twice, so pop into locals FIRST. */          \
      Value vb = pop(), va = pop();                                            \
      push(FLOAT_VAL(AS_NUM(va) op AS_NUM(vb)));                               \
    }                                                                          \
  } while (false)

// Numeric comparison: operands widen to double; the result is always bool.
#define NUM_COMPARE(op)                                                        \
  do {                                                                         \
    if (!IS_NUM(peek(0)) || !IS_NUM(peek(1))) {                                \
      runtimeError("operands must be numbers");                                \
      return INTERPRET_RUNTIME_ERROR;                                          \
    }                                                                          \
    Value vb = pop(), va = pop();                                             \
    push(BOOL_VAL(AS_NUM(va) op AS_NUM(vb)));                                  \
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
    if (vm.collectStats) { // a single predictable branch; off in the common case
      vm.opCounts[instruction]++;
      vm.instrCount++;
    }
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
      // Unary minus works on either numeric type (one-operand, so its own check).
      if (IS_INT(peek(0)))
        push(INT_VAL(-AS_INT(pop())));
      else if (IS_FLOAT(peek(0)))
        push(FLOAT_VAL(-AS_FLOAT(pop())));
      else {
        runtimeError("operand of '-' must be a number");
        return INTERPRET_RUNTIME_ERROR;
      }
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
      } else if (IS_NUM(peek(0)) && IS_NUM(peek(1))) {
        NUM_ARITH(+); // int+int -> int, otherwise float (with promotion)
      } else {
        runtimeError("operands to '+' must be two numbers or two strings");
        return INTERPRET_RUNTIME_ERROR;
      }
      break;
    case OP_SUB:
      NUM_ARITH(-);
      break;
    case OP_MUL:
      NUM_ARITH(*);
      break;
    case OP_DIV: {
      if (!IS_NUM(peek(0)) || !IS_NUM(peek(1))) {
        runtimeError("operands must be numbers");
        return INTERPRET_RUNTIME_ERROR;
      }
      if (IS_INT(peek(0)) && IS_INT(peek(1))) {
        // Integer division: guard against divide-by-zero (UB in C).
        if (AS_INT(peek(0)) == 0) {
          runtimeError("division by zero");
          return INTERPRET_RUNTIME_ERROR;
        }
        int64_t b = AS_INT(pop()), a = AS_INT(pop());
        push(INT_VAL(a / b));
      } else {
        // Float division: IEEE-defined even by zero (gives inf/nan), no error.
        Value vb = pop(), va = pop();
        push(FLOAT_VAL(AS_NUM(va) / AS_NUM(vb)));
      }
      break;
    }
    case OP_MOD: {
      // Same shape as OP_DIV: integer operands, and a zero divisor is a runtime
      // error (C's % by zero is undefined behaviour, just like /).
      if (!IS_INT(peek(0)) || !IS_INT(peek(1))) {
        runtimeError("operands must be integers");
        return INTERPRET_RUNTIME_ERROR;
      }
      if (AS_INT(peek(0)) == 0) {
        runtimeError("modulo by zero");
        return INTERPRET_RUNTIME_ERROR;
      }
      int64_t b = AS_INT(pop());
      int64_t a = AS_INT(pop());
      push(INT_VAL(a % b));
      break;
    }
    case OP_BITAND:
      BINARY_OP(INT_VAL, &);
      break;
    case OP_BITOR:
      BINARY_OP(INT_VAL, |);
      break;
    case OP_BITXOR:
      BINARY_OP(INT_VAL, ^);
      break;
    case OP_SHL:
    case OP_SHR: {
      // Shifts need integer operands AND a shift amount in [0, 63] (C leaves
      // other amounts undefined), so they can't use the bare BINARY_OP macro.
      if (!IS_INT(peek(0)) || !IS_INT(peek(1))) {
        runtimeError("operands must be integers");
        return INTERPRET_RUNTIME_ERROR;
      }
      if (AS_INT(peek(0)) < 0 || AS_INT(peek(0)) > 63) {
        runtimeError("shift amount must be between 0 and 63");
        return INTERPRET_RUNTIME_ERROR;
      }
      int64_t b = AS_INT(pop());
      int64_t a = AS_INT(pop());
      push(INT_VAL(instruction == OP_SHL ? (int64_t)((uint64_t)a << b)
                                         : a >> b));
      break;
    }
    case OP_BITNOT:
      if (!IS_INT(peek(0))) {
        runtimeError("operand of '~' must be an integer");
        return INTERPRET_RUNTIME_ERROR;
      }
      push(INT_VAL(~AS_INT(pop())));
      break;
    case OP_EQUAL: {
      // Equality is defined for ALL types (via valuesEqual), so unlike the
      // ordering comparisons it needs no integer check.
      Value b = pop();
      Value a = pop();
      push(BOOL_VAL(valuesEqual(a, b)));
      break;
    }
    case OP_LESS:
      NUM_COMPARE(<);
      break;
    case OP_GREATER:
      NUM_COMPARE(>);
      break;
    case OP_DEFINE_GLOBAL: {
      // The operand indexes the name in the constant pool. We read the value
      // from the stack, store it, then pop. Defining over an existing global is
      // allowed (it just overwrites) — a deliberate, lenient choice.
      ObjString *name = READ_STRING();
      tableSet(&vm.globals, name, peek(0));
      pop();
      // A new global may have rebuilt the table, moving entry indices — bump the
      // generation so every cached global lookup re-resolves. (Over-approximate:
      // also bumps on a plain overwrite, but defines are rare next to reads.)
      vm.globalsGen++;
      break;
    }
    case OP_GET_GLOBAL: {
      // Global inline cache: the name's constant index keys a per-chunk cache of
      // (generation, table-entry-index). On a hit we skip the hash probe and read
      // the entry directly; the generation guards against the table being rebuilt.
      uint8_t ci = READ_BYTE();
      Chunk *chunk = &frame->closure->function->chunk;
      if (chunk->globalCache == NULL) {
        chunk->globalCache =
            calloc(chunk->constants.count, sizeof(GlobalCacheSlot));
      }
      GlobalCacheSlot *slot = chunk->globalCache ? &chunk->globalCache[ci] : NULL;
      if (slot != NULL && slot->gen == vm.globalsGen) {
        push(vm.globals.entries[slot->index].value); // cache hit: no probe
        break;
      }
      ObjString *name = AS_STRING(chunk->constants.values[ci]);
      int idx = tableFindIndex(&vm.globals, name);
      if (idx >= 0 && slot != NULL) { // remember it for next time
        slot->gen = vm.globalsGen;
        slot->index = idx;
      }
      if (idx < 0) {
        // Reading a name that was never defined is a runtime error — the safety
        // guarantee that makes variables usable. (A typo'd name fails loudly,
        // with a "did you mean …?" hint when a close global name exists.)
        const char *guess = suggestGlobal(name->chars);
        if (guess != NULL)
          runtimeError("undefined variable '%s' (did you mean '%s'?)",
                       name->chars, guess);
        else
          runtimeError("undefined variable '%s'", name->chars);
        return INTERPRET_RUNTIME_ERROR;
      }
      push(vm.globals.entries[idx].value);
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
    case OP_TAIL_CALL: {
      // `return f(args)`. For a CLOSURE callee, reuse the current frame instead
      // of pushing a new one — so a tail-recursive loop runs in O(1) stack.
      int argCount = READ_BYTE();
      Value callee = peek(argCount);
      // Frame reuse applies to ordinary closures only. A generator closure must
      // go through the normal path (which builds a generator object, not a call).
      if (IS_CLOSURE(callee) && !AS_CLOSURE(callee)->function->isGenerator) {
        ObjClosure *closure = AS_CLOSURE(callee);
        if (argCount != closure->function->arity) {
          runtimeError("%s() expects %d arguments but got %d",
                       closure->function->name ? closure->function->name->chars : "fn",
                       closure->function->arity, argCount);
          return INTERPRET_RUNTIME_ERROR;
        }
        // We are abandoning the current frame: close its captured upvalues and
        // drop any try handlers it registered (none, since TCO is disabled inside
        // a try — but stay honest).
        closeUpvalues(frame->slots);
        while (vm.handlerCount > 0 &&
               vm.handlers[vm.handlerCount - 1].frameCount >= vm.frameCount)
          vm.handlerCount--;
        // Slide [callee, args...] down over the current frame's window, then
        // re-point this same frame at the callee. frameCount is unchanged.
        Value *dest = frame->slots;
        Value *src = vm.stackTop - argCount - 1;
        memmove(dest, src, sizeof(Value) * (argCount + 1));
        vm.stackTop = dest + argCount + 1;
        frame->closure = closure;
        frame->ip = closure->function->chunk.code;
        frame->slots = dest;
        break; // continue executing the callee in the reused frame
      }
      // Non-closure (native / constructor): behave like a normal call; the
      // OP_RETURN the compiler emitted next returns the result the usual way.
      if (!callValue(callee, argCount))
        return INTERPRET_RUNTIME_ERROR;
      frame = &vm.frames[vm.frameCount - 1];
      break;
    }
    case OP_INVOKE: {
      // Method call. The receiver sits `argCount` slots below the top, its
      // arguments above it. Dispatch on the receiver's type (in builtins.c); on
      // success, replace receiver+args with the single result. No frame is
      // pushed — builtin methods, like native functions, run in C.
      ObjString *method = READ_STRING();
      int argCount = READ_BYTE();
      Value receiver = peek(argCount);

      // A struct instance: dispatch to a USER method. The receiver already sits
      // `argCount` slots below the top, exactly where a call frame's slot 0 goes —
      // so calling the method closure makes slot 0 the receiver, i.e. `self`.
      if (IS_INSTANCE(receiver)) {
        ObjInstance *inst = AS_INSTANCE(receiver);
        Value m;
        if (!tableGet(&inst->type->methods, method, &m)) {
          runtimeError("%s has no method '%s'", inst->type->name->chars,
                       method->chars);
          return INTERPRET_RUNTIME_ERROR;
        }
        if (!call(AS_CLOSURE(m), argCount))
          return INTERPRET_RUNTIME_ERROR;
        frame = &vm.frames[vm.frameCount - 1]; // resume in the method
        break;
      }

      // A built-in type (string/array/map): dispatch to a native method.
      Value result;
      if (!invokeMethod(receiver, method, argCount, vm.stackTop - argCount,
                        &result))
        return INTERPRET_RUNTIME_ERROR;
      vm.stackTop -= argCount + 1; // pop the arguments and the receiver
      push(result);
      break;
    }
    case OP_BUILD_ARRAY: {
      int count = READ_BYTE();
      // Allocate the array while its elements are still on the stack (so a GC
      // triggered by this allocation keeps them alive), then copy them in.
      // writeValueArray grows with plain realloc and never collects, so the new
      // (not-yet-rooted) array can't be reclaimed before we push it.
      ObjArray *array = newArrayObject();
      for (int i = 0; i < count; i++)
        writeValueArray(&array->elements, vm.stackTop[-count + i]);
      vm.stackTop -= count;     // pop the elements
      push(OBJ_VAL(array));     // push the finished array
      break;
    }
    case OP_BUILD_MAP: {
      int pairs = READ_BYTE();
      // Allocate while the key/value pairs are still on the stack (rooted), then
      // insert them. mapSet grows with plain realloc and never collects, so the
      // new map can't be reclaimed before we push it.
      ObjMap *map = newMapObject();
      for (int i = 0; i < pairs; i++) {
        Value key = vm.stackTop[-2 * pairs + 2 * i];
        Value value = vm.stackTop[-2 * pairs + 2 * i + 1];
        if (!isHashableKey(key)) {
          runtimeError("a map key must be an int, bool, nil, or str");
          return INTERPRET_RUNTIME_ERROR;
        }
        mapSet(map, key, value);
      }
      vm.stackTop -= 2 * pairs; // pop the keys and values
      push(OBJ_VAL(map));
      break;
    }
    case OP_INDEX_GET: {
      Value index = pop();
      Value object = pop();
      if (IS_ARRAY(object)) {
        if (!IS_INT(index)) {
          runtimeError("array index must be an int");
          return INTERPRET_RUNTIME_ERROR;
        }
        ObjArray *array = AS_ARRAY(object);
        int64_t i = AS_INT(index);
        if (i < 0 || i >= array->elements.count) {
          runtimeError("array index %lld out of range (length %d)",
                       (long long)i, array->elements.count);
          return INTERPRET_RUNTIME_ERROR;
        }
        push(array->elements.values[i]);
      } else if (IS_MAP(object)) {
        if (!isHashableKey(index)) {
          runtimeError("a map key must be an int, bool, nil, or str");
          return INTERPRET_RUNTIME_ERROR;
        }
        Value value;
        if (!mapGet(AS_MAP(object), index, &value)) {
          runtimeError("key not found in map");
          return INTERPRET_RUNTIME_ERROR;
        }
        push(value);
      } else {
        runtimeError("can only index into arrays and maps");
        return INTERPRET_RUNTIME_ERROR;
      }
      break;
    }
    case OP_INDEX_SET: {
      // Stack: [.. object index value]. Store, then leave `value` as the result.
      Value value = pop();
      Value index = pop();
      Value object = pop();
      if (IS_ARRAY(object)) {
        if (!IS_INT(index)) {
          runtimeError("array index must be an int");
          return INTERPRET_RUNTIME_ERROR;
        }
        ObjArray *array = AS_ARRAY(object);
        int64_t i = AS_INT(index);
        if (i < 0 || i >= array->elements.count) {
          runtimeError("array index %lld out of range (length %d)",
                       (long long)i, array->elements.count);
          return INTERPRET_RUNTIME_ERROR;
        }
        array->elements.values[i] = value;
      } else if (IS_MAP(object)) {
        if (!isHashableKey(index)) {
          runtimeError("a map key must be an int, bool, nil, or str");
          return INTERPRET_RUNTIME_ERROR;
        }
        mapSet(AS_MAP(object), index, value); // inserts or overwrites
      } else {
        runtimeError("can only index into arrays and maps");
        return INTERPRET_RUNTIME_ERROR;
      }
      push(value);
      break;
    }
    case OP_METHOD: {
      // Install a method on the struct during its declaration. Stack: [.. struct
      // closure]; pop the closure into the struct's method table, leave struct.
      ObjString *name = READ_STRING(); // struct is peek(1); closure is peek(0)
      tableSet(&AS_STRUCT(peek(1))->methods, name, peek(0));
      pop(); // the closure; the struct stays for the next method / define
      break;
    }
    case OP_IS_KIND: {
      uint8_t tag = READ_BYTE();
      Value v = pop();
      bool r = false;
      switch (tag) {
      case IS_TAG_INT: r = IS_INT(v); break;
      case IS_TAG_BOOL: r = IS_BOOL(v); break;
      case IS_TAG_STR: r = IS_STRING(v); break;
      case IS_TAG_NIL: r = IS_NIL(v); break;
      case IS_TAG_ARRAY: r = IS_ARRAY(v); break;
      case IS_TAG_MAP: r = IS_MAP(v); break;
      }
      push(BOOL_VAL(r));
      break;
    }
    case OP_IS_STRUCT: {
      Value structVal = pop();
      Value v = pop();
      push(BOOL_VAL(IS_STRUCT(structVal) && IS_INSTANCE(v) &&
                    AS_INSTANCE(v)->type == AS_STRUCT(structVal)));
      break;
    }
    case OP_BEGIN_TRY: {
      // Register a handler: where the catch code is, and the stack/frame depth to
      // restore when unwinding to it.
      uint16_t offset = READ_SHORT();
      if (vm.handlerCount == TRY_MAX) {
        runtimeError("too many nested try blocks");
        return INTERPRET_RUNTIME_ERROR;
      }
      TryHandler *h = &vm.handlers[vm.handlerCount++];
      h->handlerIp = frame->ip + offset; // forward offset, like a jump
      h->stackTop = vm.stackTop;
      h->frameCount = vm.frameCount;
      break;
    }
    case OP_END_TRY:
      vm.handlerCount--; // body finished without throwing; drop the handler
      break;
    case OP_THROW: {
      Value thrown = pop();
      if (vm.handlerCount == 0) {
        // Uncaught: report (showing the value if it's a string) and abort.
        if (IS_STRING(thrown))
          runtimeError("uncaught exception: %s", AS_CSTRING(thrown));
        else
          runtimeError("uncaught exception");
        return INTERPRET_RUNTIME_ERROR;
      }
      // Unwind to the nearest handler: close any upvalues in the abandoned
      // region, restore the frame/stack depth, then hand the value to the catch.
      TryHandler *h = &vm.handlers[--vm.handlerCount];
      closeUpvalues(h->stackTop);
      vm.frameCount = h->frameCount;
      vm.stackTop = h->stackTop;
      push(thrown); // becomes the catch variable
      frame = &vm.frames[vm.frameCount - 1];
      frame->ip = h->handlerIp;
      break;
    }
    case OP_YIELD: {
      // Suspend the generator being resumed: freeze its live frame window + ip
      // back into the generator object, pop the frame, and hand the yielded value
      // up to resumeGenerator (which is waiting at `stopFrame`).
      Value yielded = pop();
      ObjGenerator *gen = vm.resuming;
      int count = (int)(vm.stackTop - frame->slots);
      gen->saved = realloc(gen->saved, sizeof(Value) * (count > 0 ? count : 1));
      if (gen->saved == NULL) {
        runtimeError("out of memory suspending a generator");
        return INTERPRET_RUNTIME_ERROR;
      }
      memcpy(gen->saved, frame->slots, sizeof(Value) * count);
      gen->savedCount = count;
      gen->ip = frame->ip; // resume just past this OP_YIELD
      gen->started = true;
      // Tear the frame down (closing upvalues keeps the stack safe; a closure
      // that captured a generator local across a yield is a documented gap).
      closeUpvalues(frame->slots);
      vm.frameCount--;
      vm.stackTop = frame->slots;
      push(yielded);       // the value resumeGenerator will pop
      vm.didYield = true;
      if (vm.frameCount == stopFrame)
        return INTERPRET_OK; // back to resumeGenerator
      frame = &vm.frames[vm.frameCount - 1];
      break;
    }
    case OP_GET_FIELD: {
      ObjString *name = READ_STRING();
      Value obj = pop();
      // `Enum.Member` reuses field-access: look the member up in the enum's table.
      if (IS_ENUM(obj)) {
        Value member;
        if (!tableGet(&AS_ENUM(obj)->members, name, &member)) {
          runtimeError("enum %s has no member '%s'", AS_ENUM(obj)->name->chars,
                       name->chars);
          return INTERPRET_RUNTIME_ERROR;
        }
        push(member);
        break;
      }
      if (!IS_INSTANCE(obj)) {
        runtimeError("only struct instances have fields");
        return INTERPRET_RUNTIME_ERROR;
      }
      ObjInstance *inst = AS_INSTANCE(obj);
      Value value;
      if (!tableGet(&inst->fields, name, &value)) {
        runtimeError("%s has no field '%s'", inst->type->name->chars,
                     name->chars);
        return INTERPRET_RUNTIME_ERROR;
      }
      push(value);
      break;
    }
    case OP_SET_FIELD: {
      // Stack: [.. instance value]. Store, then leave `value` as the result.
      ObjString *name = READ_STRING();
      Value value = pop();
      Value obj = pop();
      if (!IS_INSTANCE(obj)) {
        runtimeError("only struct instances have fields");
        return INTERPRET_RUNTIME_ERROR;
      }
      ObjInstance *inst = AS_INSTANCE(obj);
      if (!structHasField(inst->type, name)) {
        runtimeError("%s has no field '%s'", inst->type->name->chars,
                     name->chars);
        return INTERPRET_RUNTIME_ERROR;
      }
      tableSet(&inst->fields, name, value);
      push(value);
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
      // Discard any try handlers registered in the frame we're leaving (e.g. a
      // `return` inside a try block), so a later throw can't unwind into it.
      while (vm.handlerCount > 0 &&
             vm.handlers[vm.handlerCount - 1].frameCount > vm.frameCount)
        vm.handlerCount--;
      if (vm.frameCount == stopFrame) {
        // We've returned out of the frame our caller was waiting on.
        if (stopFrame == 0) {
          pop(); // top level: discard the script's reserved slot 0
          return INTERPRET_OK;
        }
        // A native callback (callFromVM) is waiting: leave the result on top
        // where the callee sat, for it to pop.
        vm.stackTop = frame->slots;
        push(result);
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

// Shared back end: type-check, optimise, compile and RUN an already parsed (and
// import-resolved) Program. Consumes `program` (frees it) and the type arena.
static InterpretResult runProgram(Program *program, bool trace) {
  // 1b. STATIC TYPE CHECK. A separate analysis pass over the AST that catches
  // type errors before any code runs. Gradual: unannotated code is `any` and
  // passes trivially. On failure we refuse to compile or run, like a real
  // ahead-of-time compiler rejecting an ill-typed program.
  if (!typecheckProgram(program)) {
    freeProgram(program);
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }

  // 1c. OPTIMISE the AST. Constant folding rewrites constant subexpressions into
  // literals (`2 + 3 * 4` -> `14`) so the VM never recomputes them. Runs after
  // type-checking (which used the original tree for accurate error lines) and
  // before compilation, which then emits bytecode for the simpler tree.
  foldConstants(program);

  // 2. Compile the program into a top-level ObjFunction. (Its chunk, and every
  // nested function's chunk, is owned by the VM object list — freed at shutdown,
  // not here.)
  ObjFunction *function = compile(program);
  freeProgram(program);    // trees no longer needed once bytecode exists
  freeTypes();             // ...and the type arena: bytecode doesn't reference types
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

  // Execution is about to begin: the operand stack, frames and globals are now
  // valid GC roots, and the AST has been freed. Safe to turn the collector on.
  vm.gcEnabled = true;
  return run(trace, /*stopFrame=*/0);
}

InterpretResult interpret(const char *source, bool trace) {
  // Keep the collector off across the whole front end (parse/type-check/compile),
  // which allocates AST-referenced strings that aren't GC roots yet. Matters in
  // the REPL, where interpret() is re-entered per line after GC was switched on.
  vm.gcEnabled = false;

  // 1. Parse, then splice in any `import`ed files (relative to the CWD here).
  Program program;
  if (!loadModuleSource(source, &program)) {
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }
  return runProgram(&program, trace);
}

InterpretResult interpretFile(const char *path, bool trace) {
  vm.gcEnabled = false;
  // Parse `path` and resolve its imports relative to the file's own directory.
  Program program;
  if (!loadModuleFile(path, &program)) {
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }
  return runProgram(&program, trace);
}

bool callFromVM(Value callee, Value *args, int argCount, Value *result) {
  // Push the callee and its arguments, then dispatch. We remember the current
  // frame depth so we know when "our" call has returned.
  int stop = vm.frameCount;
  push(callee);
  for (int i = 0; i < argCount; i++)
    push(args[i]);
  if (!callValue(callee, argCount))
    return false; // arity/callability error already reported

  if (vm.frameCount == stop) {
    // A native callee ran inline (no frame pushed); its result is already on top.
    *result = pop();
    return true;
  }
  // A cnano closure frame was pushed: run until it returns back to our depth.
  if (run(false, stop) != INTERPRET_OK)
    return false;
  *result = pop();
  return true;
}

bool resumeGenerator(ObjGenerator *gen, Value *result, bool *finished) {
  if (gen->done) { // already exhausted: keep yielding nil
    *result = NIL_VAL;
    *finished = true;
    return true;
  }
  if (vm.stackTop - vm.stack + gen->savedCount > STACK_MAX ||
      vm.frameCount == FRAMES_MAX) {
    runtimeError("stack overflow resuming a generator");
    return false;
  }
  // Thaw the frozen window onto the top of the stack and rebuild its frame.
  Value *base = vm.stackTop;
  memcpy(base, gen->saved, sizeof(Value) * gen->savedCount);
  vm.stackTop += gen->savedCount;
  int stop = vm.frameCount;
  CallFrame *frame = &vm.frames[vm.frameCount++];
  frame->closure = gen->closure;
  frame->ip = gen->ip; // code start on the first resume; past the yield after
  frame->slots = base;

  // Bracket the resume state so nested generator resumes restore cleanly.
  ObjGenerator *savedResuming = vm.resuming;
  bool savedDidYield = vm.didYield;
  vm.resuming = gen;
  vm.didYield = false;
  InterpretResult r = run(false, stop);
  bool yielded = vm.didYield;
  vm.resuming = savedResuming;
  vm.didYield = savedDidYield;
  if (r != INTERPRET_OK)
    return false;
  *result = pop();        // the yielded value, or (on return) the return value
  *finished = !yielded;   // a return (not a yield) means the generator is done
  if (!yielded)
    gen->done = true;
  return true;
}

// Shared native back end: type-check, fold and emit C for a parsed (and
// import-resolved) Program. Consumes `program` and the type arena.
static InterpretResult emitProgram(Program *program, FILE *cFile) {
  // Identical front end to runProgram(): type-check, fold. Reusing it means the
  // native path accepts exactly the programs the VM does (and rejects the same
  // errors), differing only in the BACKEND it feeds the AST to.
  if (!typecheckProgram(program)) {
    freeProgram(program);
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }
  foldConstants(program);

  bool gen = emitC(program, cFile);
  freeProgram(program);
  freeTypes();
  return gen ? INTERPRET_OK : INTERPRET_COMPILE_ERROR;
}

InterpretResult compileToC(const char *source, FILE *cFile) {
  vm.gcEnabled = false; // parsing interns strings into the (un-rooted) VM
  Program program;
  if (!loadModuleSource(source, &program)) {
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }
  return emitProgram(&program, cFile);
}

InterpretResult compileFileToC(const char *path, FILE *cFile) {
  vm.gcEnabled = false;
  Program program;
  if (!loadModuleFile(path, &program)) {
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }
  return emitProgram(&program, cFile);
}

// Print the control-flow graph of a function and, recursively, of every nested
// function it defines (which ride in its constant pool).
static void cfgForFunction(ObjFunction *fn) {
  CFG *cfg = buildCFG(&fn->chunk);
  printCFG(cfg, fn->name ? fn->name->chars : "<script>");
  freeCFG(cfg);
  for (int i = 0; i < fn->chunk.constants.count; i++)
    if (IS_FUNCTION(fn->chunk.constants.values[i]))
      cfgForFunction(AS_FUNCTION(fn->chunk.constants.values[i]));
}

// Compile FILE to three-address IR and print it (`--ir`). We deliberately do NOT
// run the AST constant-folder first, so the IR optimiser has constants to fold
// and the before/after is meaningful.
InterpretResult dumpIRFile(const char *path) {
  vm.gcEnabled = false;
  Program program;
  if (!loadModuleFile(path, &program)) {
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }
  if (!typecheckProgram(&program)) {
    freeProgram(&program);
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }
  // The top-level straight-line code.
  IRFunc *top = lowerToIR(&program, "<script>");
  if (top != NULL) {
    optimizeIR(top); // prints the IR before (lowered) and after (optimised)
    freeIR(top);
  } else {
    printf("== IR: <script> == (top level is not straight-line scalar code)\n\n");
  }
  // Each top-level function whose body is straight-line scalar code.
  for (int i = 0; i < program.count; i++) {
    Node *s = program.statements[i];
    if (s->type != NODE_FUN)
      continue;
    const char *name = s->as.fun.name ? s->as.fun.name->chars : "fn";
    IRFunc *fn = lowerToIR(s->as.fun.body, name);
    if (fn != NULL) {
      optimizeIR(fn); // prints the IR before (lowered) and after (optimised)
      freeIR(fn);
    } else {
      printf("== IR: %s == (not straight-line scalar code; skipped)\n\n", name);
    }
  }
  freeProgram(&program);
  freeTypes();
  return INTERPRET_OK;
}

// `--asm`: lower the top-level straight-line integer code to IR, optimise it,
// and emit x86-64 assembly to stdout (which `cc` can assemble into a binary).
InterpretResult dumpAsmFile(const char *path) {
  vm.gcEnabled = false;
  Program program;
  if (!loadModuleFile(path, &program)) {
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }
  if (!typecheckProgram(&program)) {
    freeProgram(&program);
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }
  IRFunc *top = lowerToIR(&program, "<script>");
  InterpretResult result = INTERPRET_OK;
  if (top == NULL) {
    fprintf(stderr,
            "cnano: the x86-64 backend only supports straight-line scalar code "
            "(no control flow, calls, or collections).\n");
    result = INTERPRET_COMPILE_ERROR;
  } else {
    // Emit from the LOWERED (unoptimised) IR on purpose: on literal-only
    // straight-line code the optimiser folds everything to constants, which would
    // hide the instruction selection and register allocation this backend exists
    // to show. (`--ir` already demonstrates the optimiser; running
    // optimizeIRPasses(top) here first would simply produce tighter code.)
    if (!emitX64(top, stdout)) {
      fprintf(stderr, "cnano: the x86-64 backend only supports integer code "
                      "(no bool/float values).\n");
      result = INTERPRET_COMPILE_ERROR;
    }
    freeIR(top);
  }
  freeProgram(&program);
  freeTypes();
  return result;
}

// `--types`: type-check (which runs return-type inference) and print the static
// type of every top-level binding — the inference made visible, like `--ir`.
InterpretResult dumpTypesFile(const char *path) {
  vm.gcEnabled = false;
  Program program;
  if (!loadModuleFile(path, &program)) {
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }
  if (!typecheckProgram(&program)) {
    freeProgram(&program);
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }
  printf("== types ==\n");
  for (int i = 0; i < program.count; i++) {
    Node *s = program.statements[i];
    if (s->type == NODE_VAR_DECL) {
      const char *t = s->as.var.inferredType ? typeName(s->as.var.inferredType) : "any";
      printf("  %s %s : %s\n", s->as.var.isConst ? "const" : "let",
             s->as.var.name->chars, t);
    } else if (s->type == NODE_FUN) {
      printf("  fn %s(", s->as.fun.name ? s->as.fun.name->chars : "lambda");
      for (int p = 0; p < s->as.fun.paramCount; p++)
        printf("%s%s: %s", p ? ", " : "", s->as.fun.params[p]->chars,
               typeName(s->as.fun.paramTypes[p]));
      // returnType was rewritten in place to the inferred type when unannotated.
      printf(") : %s%s\n", typeName(s->as.fun.returnType),
             s->as.fun.returnAnnotated ? "" : "   (inferred)");
    }
  }
  printf("\n");
  freeProgram(&program);
  freeTypes();
  return INTERPRET_OK;
}

InterpretResult dumpCFGFile(const char *path) {
  vm.gcEnabled = false;
  Program program;
  if (!loadModuleFile(path, &program)) {
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }
  if (!typecheckProgram(&program)) {
    freeProgram(&program);
    freeTypes();
    return INTERPRET_COMPILE_ERROR;
  }
  foldConstants(&program);
  ObjFunction *function = compile(&program);
  freeProgram(&program);
  freeTypes();
  if (function == NULL)
    return INTERPRET_COMPILE_ERROR;
  cfgForFunction(function);
  return INTERPRET_OK;
}
