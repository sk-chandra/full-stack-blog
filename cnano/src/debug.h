// debug.h — a disassembler that prints bytecode in human-readable form.
//
// A disassembler is the single most useful tool when building a VM: it lets you
// SEE the instructions your compiler produced and confirm they are what you
// expected. Run `cnano --dump file.cn` to print the chunk before it executes.
#ifndef CNANO_DEBUG_H
#define CNANO_DEBUG_H

#include "chunk.h"

// Print every instruction in `chunk`, prefixed by a `name` banner.
void disassembleChunk(Chunk *chunk, const char *name);
// Print the single instruction at byte `offset`; returns the offset of the next
// instruction. Exposed so the VM can trace execution one step at a time.
int disassembleInstruction(Chunk *chunk, int offset);

#endif // CNANO_DEBUG_H
