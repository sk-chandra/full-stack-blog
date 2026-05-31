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
  OP_RETURN,   // [opcode]        : end execution; result is top of stack
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
