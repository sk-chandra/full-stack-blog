// codegen_x64.h — a tiny x86-64 assembly backend for the IR (step 69).
//
// This is where cnano's compiler finally reaches real machine code. It takes the
// three-address IR (the representation built in Arc 6) and emits AT&T-syntax
// x86-64 assembly, doing the two jobs a back end must: INSTRUCTION SELECTION
// (each IR op → one or two machine instructions) and REGISTER ALLOCATION
// (mapping the IR's unbounded temporaries onto the CPU's handful of registers,
// by linear scan, spilling to the stack when they don't fit). The result
// assembles and links with `cc` into a native executable that runs with no cnano
// runtime at all.
//
// Scope: the straight-line INTEGER subset (the IR's domain) — int constants,
// variables, +,-,*,/,%, bitwise, shifts, unary -/~, and `print`. Booleans,
// floats, control flow, and calls are out of scope and cleanly REJECTED (the IR
// either never produces them or the emitter returns false), the same honesty as
// the --native C backend.
#ifndef CNANO_CODEGEN_X64_H
#define CNANO_CODEGEN_X64_H

#include <stdio.h>

#include "ir.h"

// Emit a complete assembly program for an IR module — `main` plus one function
// per definition (calls follow the System V ABI) — to `out`. Returns false
// without emitting if anything is outside the supported integer subset, so the
// caller can report the program unsupported.
bool emitX64Module(IRModule *m, FILE *out);

#endif // CNANO_CODEGEN_X64_H
