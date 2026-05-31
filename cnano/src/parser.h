// parser.h — turns a token stream into an AST.
//
// cnano uses *recursive descent*: one C function per grammar rule, and the call
// stack itself mirrors the nesting of the expression. It is the most widely
// taught parsing technique because the code reads almost exactly like the
// grammar it implements.
#ifndef CNANO_PARSER_H
#define CNANO_PARSER_H

#include "ast.h"

// Parse `source` into an expression tree. Returns NULL on a syntax error
// (after printing a message). The caller owns the returned tree and must
// freeNode() it.
Node *parse(const char *source);

#endif // CNANO_PARSER_H
