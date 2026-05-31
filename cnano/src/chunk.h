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
  // Statement-level opcodes. Unlike the operators above, these consume a value
  // WITHOUT pushing one back — they exist for their effect on output or the
  // stack, mirroring the expression/statement split in the language itself.
  OP_PRINT,    // [opcode]        : pop one value and print it, then a newline
  OP_POP,      // [opcode]        : pop one value and discard it
  OP_RETURN,   // [opcode]        : end execution (no value)
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
