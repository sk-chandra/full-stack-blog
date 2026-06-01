// module.h — the module loader: resolving `import "path";` into one program.
//
// cnano programs can be split across files. An `import "other.cn";` at the top
// level pulls in another file's declarations. Rather than teach the type checker,
// compiler and VM about multiple files, we resolve imports as a SEPARATE pass
// that runs right after parsing: each imported file is parsed and its top-level
// statements are spliced into a single merged Program, which the rest of the
// pipeline then consumes unchanged. This "flatten to one translation unit" model
// is the simplest thing that teaches the core ideas — path resolution, include
// ordering, and once-only loading (so diamonds and cycles are safe).
#ifndef CNANO_MODULE_H
#define CNANO_MODULE_H

#include "ast.h"

// Read an entire file into a heap buffer (NUL-terminated). The caller frees it.
// Exits the process with an IO error code if the file cannot be read.
char *readFileOrExit(const char *path);

// Load the file at `path`, recursively resolve its imports (relative to each
// importing file's own directory), and produce a single merged Program in *out.
// Each distinct file (by canonical path) is included at most once. Returns false
// on a parse or IO error, having printed a message; *out is not left allocated.
bool loadModuleFile(const char *path, Program *out);

// Like loadModuleFile but for source text with no backing file (the REPL and
// piped input). Imports resolve relative to the current working directory.
bool loadModuleSource(const char *source, Program *out);

// --- multi-file source positions -------------------------------------------
// To attribute an error to the right file after several files are spliced into
// one program, each file's lines live in their own band of the line-number space
// (file index * CNANO_FILE_SPAN + local line). The loader registers each file as
// it loads it; the error printers format a (possibly banded) line back into a
// human location, naming the file only when more than one is in play.
#define CNANO_FILE_SPAN 1000000

// Register a file's display name (load order = index) and return the base line
// offset its lexer should start at (index * CNANO_FILE_SPAN).
int moduleRegisterFile(const char *displayName);
// Write the location of a (possibly banded) line into `buf`: "5" when a single
// file is in play, or "name.cn:5" when several are. The caller wraps it, e.g.
// "[%s] Error".
void moduleFormatLine(int line, char *buf, int size);
// The within-file line for a banded global line (for rendering a source caret).
int moduleLocalLine(int line);

#endif // CNANO_MODULE_H
