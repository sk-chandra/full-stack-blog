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
#include "object.h"

// Compile a whole `program` into a top-level ObjFunction (an implicit "main").
// Returns NULL if a compile-time error occurred (the message is printed). The
// returned function — and every nested function it references — is owned by the
// VM's object list and freed at shutdown.
ObjFunction *compile(Program *program);

#endif // CNANO_COMPILER_H
