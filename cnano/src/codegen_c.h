// codegen_c.h — the NATIVE backend: compile cnano to C source, then to a binary.
//
// This is the leap from "interpreted" to "compiled, ahead of time". Instead of
// emitting bytecode for our VM, we emit C SOURCE for a real C compiler, which
// turns it into a native executable. Transpiling to C is a legitimate, widely
// used technique (Nim, Vala, early C++ all did/do it): C acts as a portable
// assembler, and we inherit the system compiler's optimiser and every CPU it
// targets — for a fraction of the effort of emitting machine code directly.
//
// Scope: the STATICALLY-TYPED, FIRST-ORDER subset. With types known (int, bool,
// str) we emit *unboxed* C values — int64_t, bool, const char* — so the generated
// code is genuinely fast native code, not an interpreter in disguise. Features
// outside this subset (closures, dynamic `any`, first-class functions) are
// rejected by the native backend with a clear message; the bytecode VM still runs
// the full language. This division — a fast specialised AOT path plus a general
// dynamic path — mirrors how real systems (e.g. JITs, `@nogc` subsets) are built.
#ifndef CNANO_CODEGEN_C_H
#define CNANO_CODEGEN_C_H

#include <stdio.h>

#include "ast.h"

// Emit C source for `program` to `out`. Returns true on success; on an
// unsupported construct it prints a message and returns false. The program is
// assumed to have already passed parsing, type-checking, and folding.
bool emitC(Program *program, FILE *out);

#endif // CNANO_CODEGEN_C_H
