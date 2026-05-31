// compiler.h — walks the AST and emits bytecode into a Chunk.
//
// This is the "back end" of our tiny compiler. It performs a post-order walk of
// the tree: to compile `a + b`, first compile `a` (leaves its value on the
// stack), then compile `b`, then emit OP_ADD (which pops both and pushes the
// sum). That ordering is exactly what a stack machine needs, and it is why a
// tree maps so cleanly onto stack-based bytecode.
#ifndef CNANO_COMPILER_H
#define CNANO_COMPILER_H

#include "ast.h"
#include "chunk.h"

// Compile a whole `program` (its sequence of statements) into `chunk`. The
// chunk should already be initialised.
void compile(Program *program, Chunk *chunk);

#endif // CNANO_COMPILER_H
