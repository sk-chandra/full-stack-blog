// codegen_elf.h — emit a native ELF executable DIRECTLY (step 80).
//
// `cnano --elf file.cn -o prog` produces a runnable Linux x86-64 binary with no
// assembler, no linker, and no libc: cnano itself encodes the machine-code
// BYTES (REX prefixes, ModRM, rel32 branches — the assembler's job), lays out
// and backpatches every jump and call (the linker's job), writes the ELF
// header and program headers (the loader's contract), and talks to the kernel
// with raw `write`/`exit` syscalls instead of printf. Where `--asm` teaches
// register allocation and instruction *selection*, this teaches instruction
// *encoding* and what an executable file actually is.
//
// Scope: the integer/boolean subset (control flow, functions, recursion).
// Floats are rejected — their printing needs libc's %g — as is anything the
// type-class analysis can't pin down (same honesty as the other backends).
#ifndef CNANO_CODEGEN_ELF_H
#define CNANO_CODEGEN_ELF_H

#include <stdio.h>

#include "ir.h"

// Write a complete ELF64 executable for the module to `path` (and mark it
// executable). Returns false without writing if the module is out of subset.
bool emitElfExecutable(IRModule *m, const char *path);

#endif // CNANO_CODEGEN_ELF_H
