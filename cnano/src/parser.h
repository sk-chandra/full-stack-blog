// parser.h — turns a token stream into an AST.
//
// cnano uses *recursive descent*: one C function per grammar rule, and the call
// stack itself mirrors the nesting of the expression. It is the most widely
// taught parsing technique because the code reads almost exactly like the
// grammar it implements.
#ifndef CNANO_PARSER_H
#define CNANO_PARSER_H

#include "ast.h"

// Parse `source` into a Program (a list of statements). Returns true on
// success, filling *out; returns false on a syntax error (after printing a
// message). On either result the caller owns *out and must freeProgram() it —
// even on failure it may hold partially-built statements.
bool parse(const char *source, Program *out);

#endif // CNANO_PARSER_H
