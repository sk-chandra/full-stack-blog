// chunk.h — bytecode container and the instruction set (ISA).
//
// A "chunk" is one unit of compiled code: a flat array of bytes (the
// instructions), a parallel array of source line numbers (for error messages),
// and the constant pool. Designing the OpCode enum below *is* designing the
// instruction set of our virtual machine — the lowest-level "language" cnano
// actually executes.
#ifndef CNANO_CHUNK_H
#define CNANO_CHUNK_H

#include "common.h"
#include "value.h"

// The instruction set. Every opcode is one byte. Some are followed by operand
// bytes embedded in the stream (only OP_CONSTANT here, which takes a 1-byte
// index). This is a *stack machine*: arithmetic ops take their inputs from the
// top of an operand stack and push their result back. Stack machines are easy
// to compile to and easy to write, which is why so many bytecode VMs use them.
typedef enum {
  OP_CONSTANT, // [opcode][index] : push constants[index] onto the stack
  // Same as OP_CONSTANT but with a THREE-byte (24-bit) index, so a chunk can
  // hold more than 256 constants. The compiler emits the short form when it can
  // and falls back to this only when needed — small code in the common case,
  // correctness in the rare one. (Resolves the limit flagged back in step 0.)
  OP_CONSTANT_LONG, // [opcode][hi][mid][lo] : push constants[24-bit index]
  // Dedicated opcodes for the three literal values that have no operand. We
  // *could* store true/false/nil in the constant pool like integers, but giving
  // them their own one-byte opcodes is smaller and faster — a common micro
  // optimisation in real bytecode sets (the JVM has iconst_0, aconst_null, ...).
  OP_NIL,      // [opcode]        : push nil
  OP_TRUE,     // [opcode]        : push true
  OP_FALSE,    // [opcode]        : push false
  OP_NEGATE,   // [opcode]        : a = pop;          push -a   (ints only)
  OP_NOT,      // [opcode]        : a = pop;          push logical-not of a
  OP_ADD,      // [opcode]        : b = pop; a = pop; push a + b
  OP_SUB,      // [opcode]        : b = pop; a = pop; push a - b
  OP_MUL,      // [opcode]        : b = pop; a = pop; push a * b
  OP_DIV,      // [opcode]        : b = pop; a = pop; push a / b
  OP_MOD,      // [opcode]        : b = pop; a = pop; push a % b  (b != 0)
  OP_EQUAL,    // [opcode]        : b = pop; a = pop; push (a == b)  (any types)
  OP_LESS,     // [opcode]        : b = pop; a = pop; push (a < b)   (ints only)
  OP_GREATER,  // [opcode]        : b = pop; a = pop; push (a > b)   (ints only)
  // Global variables. Each carries a 1-byte constant-pool index that points at
  // the variable's NAME (an ObjString stored as a constant). The VM uses that
  // name as a key into its `globals` hash table. Storing names in the constant
  // pool — rather than, say, resolving them to numeric slots now — is the simple
  // approach for globals; step 4 will resolve LOCALS to stack slots for speed.
  OP_DEFINE_GLOBAL, // [opcode][nameIdx] : pop value, create globals[name]=value
  OP_GET_GLOBAL,    // [opcode][nameIdx] : push globals[name] (error if undefined)
  OP_SET_GLOBAL,    // [opcode][nameIdx] : globals[name]=peek (error if undefined)
  // Local variables. Unlike globals (looked up by name in a hash table at
  // runtime), locals are resolved to a numeric STACK SLOT by the compiler, so
  // these carry a 1-byte slot index and need no name and no lookup — just a
  // direct array access. That speed difference is the whole point of step 4.
  OP_GET_LOCAL,     // [opcode][slot]    : push stack[slot]
  OP_SET_LOCAL,     // [opcode][slot]    : stack[slot] = peek(0)  (no pop)
  // Upvalues: variables a closure captured from an enclosing function. Like
  // locals these are slot-indexed (into the closure's upvalue array), but they
  // reach a variable that may now live on the heap. See compiler.c / vm.c.
  OP_GET_UPVALUE,   // [opcode][idx]     : push *closure->upvalues[idx]->location
  OP_SET_UPVALUE,   // [opcode][idx]     : *upvalues[idx]->location = peek(0)
  // Control flow. These change the instruction pointer instead of (or as well
  // as) touching the stack — they are how `if`, `while`, `for`, and `and`/`or`
  // are built. Their operand is a TWO-byte big-endian offset, so a single jump
  // can span up to 65535 bytes of bytecode (a one-byte offset would cap loops
  // and conditionals at 256 bytes — too small). The compiler fills these offsets
  // in by "backpatching" (see compiler.c).
  OP_JUMP,          // [opcode][hi][lo] : ip += offset  (unconditional forward)
  OP_JUMP_IF_FALSE, // [opcode][hi][lo] : if peek(0) is falsey, ip += offset
                    //                    (does NOT pop — the compiler pops)
  OP_LOOP,          // [opcode][hi][lo] : ip -= offset  (unconditional backward)
  // Statement-level opcodes. Unlike the operators above, these consume a value
  // WITHOUT pushing one back — they exist for their effect on output or the
  // stack, mirroring the expression/statement split in the language itself.
  OP_PRINT,    // [opcode]        : pop one value and print it, then a newline
  OP_POP,      // [opcode]        : pop one value and discard it
  // Calls. OP_CALL's operand is the ARGUMENT COUNT; the callee and its arguments
  // are already on the stack below the current top. OP_RETURN now returns from
  // the current function (popping its call frame), with the return value on top.
  OP_CALL,     // [opcode][argc]  : call the function sitting under `argc` args
  // Method call: `receiver.name(args)`. The receiver sits `argc` slots below the
  // top with its arguments above it. [nameIdx] is the method-name string in the
  // constant pool; the VM dispatches on the RECEIVER's type to a builtin method.
  // A fused get-property-then-call, like clox's OP_INVOKE — but for built-in
  // types (strings, and later arrays/maps) since cnano has no user-defined ones.
  OP_INVOKE,   // [opcode][nameIdx][argc] : call builtin method `name` on a receiver
  // Aggregate data. OP_BUILD_ARRAY pops `count` values (pushed left-to-right) and
  // pushes a new array of them. OP_INDEX_GET pops an index then an object and
  // pushes object[index]; OP_INDEX_SET expects [.. object index value], stores
  // value at object[index], pops all three and pushes value (assignment's result).
  OP_BUILD_ARRAY, // [opcode][count] : push a new array of the top `count` values
  // OP_BUILD_MAP's operand is the PAIR count; the top 2*count stack values are
  // key0 value0 key1 value1 ... (pushed in that order). OP_INDEX_GET/SET are
  // shared with arrays — the VM dispatches on the object's type.
  OP_BUILD_MAP,   // [opcode][pairs] : push a new map of the top 2*`pairs` values
  OP_INDEX_GET,   // [opcode]        : push object[index]   (arrays + maps)
  OP_INDEX_SET,   // [opcode]        : object[index] = value; push value
  // Create a closure from the function constant at [idx], then read 2 bytes per
  // upvalue describing where each capture comes from: [isLocal][index]. This is
  // our only VARIABLE-LENGTH instruction — its size depends on the function's
  // upvalue count. See the disassembler and VM for the decode.
  OP_CLOSURE,  // [opcode][idx] ( [isLocal][index] )* : push a new closure
  // Move the top-of-stack local off the stack onto the heap (close its upvalue),
  // then pop it. Emitted when a captured local goes out of scope.
  OP_CLOSE_UPVALUE, // [opcode]   : close the upvalue for the top stack slot, pop
  OP_RETURN,   // [opcode]        : return top-of-stack from the current function
} OpCode;

typedef struct {
  int count;          // number of bytes used
  int capacity;       // number of bytes allocated
  uint8_t *code;      // the instruction bytes
  int *lines;         // lines[i] = source line that produced code[i]
  ValueArray constants; // the constant pool for this chunk
} Chunk;

void initChunk(Chunk *chunk);
void freeChunk(Chunk *chunk);
// Append one byte (an opcode or an operand). `line` records where it came from.
void writeChunk(Chunk *chunk, uint8_t byte, int line);
// Add a constant to the pool and return its index, for use as an OP_CONSTANT operand.
int addConstant(Chunk *chunk, Value value);

#endif // CNANO_CHUNK_H
