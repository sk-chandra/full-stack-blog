// codegen_x64.h — the x86-64 assembly backend for the IR (steps 69–72, 78).
//
// This is where cnano's compiler reaches real machine code. It takes the
// three-address IR (the representation built in Arc 6) and emits AT&T-syntax
// x86-64 assembly, doing the jobs a back end must: INSTRUCTION SELECTION
// (each IR op → one or two machine instructions), REGISTER ALLOCATION
// (mapping the IR's unbounded temporaries onto the CPU's handful of registers,
// by linear scan, spilling to the stack when they don't fit), and a TYPE-CLASS
// analysis (int/bool/float — machine code has no runtime tags). The result
// assembles and links with `cc` into a native executable that runs with no
// cnano runtime at all, whose output matches the VM byte-for-byte.
//
// Scope: scalar ints, bools and floats; `if`/`while` control flow; functions
// with up to six parameters (recursion included), under the System V ABI.
// Anything else — collections, closures, a value whose class differs across
// paths — is cleanly REJECTED (emitX64Module returns false), the same honesty
// as the --native C backend.
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
