#include <stdio.h>
#include <stdlib.h>

#include "ast.h"
#include "lexer.h"
#include "parser.h"

// The grammar we implement, written in EBNF. Each rule below becomes a function.
//
// A program is now a SEQUENCE OF STATEMENTS, not a single expression. The top of
// the grammar is therefore about statements; expressions sit underneath:
//
//   program    -> statement* EOF ;
//   statement  -> printStmt | exprStmt ;
//   printStmt  -> "print" expression ";" ;
//   exprStmt   -> expression ";" ;
//
// Lower expression rules bind tighter (higher precedence):
//
//   expression -> equality ;
//   equality   -> comparison ( ( "==" | "!=" ) comparison )* ;
//   comparison -> term ( ( "<" | "<=" | ">" | ">=" ) term )* ;
//   term       -> factor ( ( "+" | "-" ) factor )* ;       // left-associative
//   factor     -> unary  ( ( "*" | "/" ) unary  )* ;       // left-associative
//   unary      -> ( "-" | "!" ) unary | primary ;
//   primary    -> INT | "true" | "false" | "nil" | "(" expression ")" ;
//
// Precedence falls out of the call chain: equality calls comparison calls term
// calls factor calls unary calls primary. Each new level we add sits ABOVE the
// arithmetic it should bind looser than — so `1 + 2 == 3` parses as
// `(1 + 2) == 3`, which is what people expect. Associativity falls out of the
// loops (`while`): we fold left-to-right, so 1-2-3 parses as (1-2)-3.
//
// DESUGARING: the grammar lists `!=`, `<=`, `>=`, but the AST has no operators
// for them. The parser rewrites them into the three primitives the rest of the
// pipeline supports:
//     a != b   becomes   !(a == b)
//     a <= b   becomes   !(a > b)
//     a >= b   becomes   !(a < b)
// This is "syntactic sugar": surface syntax that expands into a simpler core.
// Doing it once, here, means the compiler and VM never need to know these forms
// exist — a recurring strategy for keeping a language's CORE small while its
// SURFACE stays convenient.

typedef struct {
  Token current;  // the next token to consume (one-token lookahead)
  Token previous; // the most recently consumed token
  bool hadError;  // did ANY error occur during the whole parse?
  bool panicMode; // are we currently recovering from an error?
} Parser;

static Parser parser;

// --- error reporting -------------------------------------------------------

static void errorAt(Token *token, const char *message) {
  // panicMode suppresses the cascade of bogus errors that follows a real one,
  // until we resynchronise at a statement boundary (see synchronize()). Without
  // this, one mistake produces a wall of confusing messages. hadError stays set
  // for the whole parse so the caller knows compilation must be aborted.
  if (parser.panicMode)
    return;
  parser.panicMode = true;
  parser.hadError = true;
  fprintf(stderr, "[line %d] Error", token->line);
  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token->type == TOKEN_ERROR) {
    // The lexer already put a message in the token's text.
  } else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }
  fprintf(stderr, ": %s\n", message);
}

// --- token cursor ----------------------------------------------------------

// Pull the next real token, surfacing any lexer error tokens as parse errors.
static void advance(void) {
  parser.previous = parser.current;
  for (;;) {
    parser.current = scanToken();
    if (parser.current.type != TOKEN_ERROR)
      break;
    errorAt(&parser.current, parser.current.start);
  }
}

// Consume the current token if it matches; otherwise report `message`.
static void consume(TokenType type, const char *message) {
  if (parser.current.type == type) {
    advance();
    return;
  }
  errorAt(&parser.current, message);
}

static bool check(TokenType type) { return parser.current.type == type; }

// If the current token is `type`, consume it and return true.
static bool match(TokenType type) {
  if (!check(type))
    return false;
  advance();
  return true;
}

// --- grammar rules (forward declarations, since they call each other) -------

static Node *expression(void);
static Node *equality(void);
static Node *comparison(void);
static Node *term(void);
static Node *factor(void);
static Node *unary(void);
static Node *primary(void);

static Node *expression(void) { return equality(); }

static Node *equality(void) {
  Node *node = comparison();
  while (check(TOKEN_EQUAL_EQUAL) || check(TOKEN_BANG_EQUAL)) {
    int line = parser.current.line;
    bool negate = check(TOKEN_BANG_EQUAL); // remember before consuming
    advance();
    Node *right = comparison();
    node = newBinary(OP_NODE_EQUAL, node, right, line);
    // Desugar `a != b` into `!(a == b)`.
    if (negate)
      node = newUnary(OP_NODE_NOT, node, line);
  }
  return node;
}

static Node *comparison(void) {
  Node *node = term();
  while (check(TOKEN_LESS) || check(TOKEN_LESS_EQUAL) ||
         check(TOKEN_GREATER) || check(TOKEN_GREATER_EQUAL)) {
    int line = parser.current.line;
    TokenType op = parser.current.type;
    advance();
    Node *right = term();
    // Build each form out of the two primitives `<` and `>` plus `!`:
    switch (op) {
    case TOKEN_LESS:
      node = newBinary(OP_NODE_LESS, node, right, line);
      break;
    case TOKEN_GREATER:
      node = newBinary(OP_NODE_GREATER, node, right, line);
      break;
    case TOKEN_LESS_EQUAL: // a <= b  ==>  !(a > b)
      node = newUnary(OP_NODE_NOT,
                      newBinary(OP_NODE_GREATER, node, right, line), line);
      break;
    case TOKEN_GREATER_EQUAL: // a >= b  ==>  !(a < b)
      node = newUnary(OP_NODE_NOT,
                      newBinary(OP_NODE_LESS, node, right, line), line);
      break;
    default:
      break; // unreachable
    }
  }
  return node;
}

static Node *term(void) {
  Node *node = factor();
  while (check(TOKEN_PLUS) || check(TOKEN_MINUS)) {
    int line = parser.current.line;
    NodeOp op = match(TOKEN_PLUS) ? OP_NODE_ADD : (advance(), OP_NODE_SUB);
    Node *right = factor();
    node = newBinary(op, node, right, line);
  }
  return node;
}

static Node *factor(void) {
  Node *node = unary();
  while (check(TOKEN_STAR) || check(TOKEN_SLASH)) {
    int line = parser.current.line;
    NodeOp op = match(TOKEN_STAR) ? OP_NODE_MUL : (advance(), OP_NODE_DIV);
    Node *right = unary();
    node = newBinary(op, node, right, line);
  }
  return node;
}

static Node *unary(void) {
  // Both prefix operators recurse into unary() (not primary) so stacked prefixes
  // like `--5` or `!!true` parse right-to-left as -(-5) / !(!true).
  if (match(TOKEN_MINUS)) {
    int line = parser.previous.line;
    return newUnary(OP_NODE_NEGATE, unary(), line);
  }
  if (match(TOKEN_BANG)) {
    int line = parser.previous.line;
    return newUnary(OP_NODE_NOT, unary(), line);
  }
  return primary();
}

static Node *primary(void) {
  if (match(TOKEN_NUMBER)) {
    // strtoll parses the slice of source text the token points at. The token is
    // not NUL-terminated on its own, but it is followed by more source (or the
    // final '\0'), and strtoll stops at the first non-digit, so this is safe.
    int64_t value = strtoll(parser.previous.start, NULL, 10);
    return newInt(value, parser.previous.line);
  }
  if (match(TOKEN_TRUE))
    return newBool(true, parser.previous.line);
  if (match(TOKEN_FALSE))
    return newBool(false, parser.previous.line);
  if (match(TOKEN_NIL))
    return newNil(parser.previous.line);
  if (match(TOKEN_LPAREN)) {
    Node *node = expression();
    consume(TOKEN_RPAREN, "Expect ')' after expression.");
    return node;
  }
  errorAt(&parser.current, "Expect a value or '('.");
  return NULL;
}

// --- statements ------------------------------------------------------------

static Node *statement(void);

// After a syntax error we "panic": skip tokens until we reach a likely
// statement boundary, so parsing can resume and report further independent
// errors instead of one cascade. We stop just after a ';' (the end of the bad
// statement) or just before a token that clearly begins a new statement. This
// is classic panic-mode error recovery.
static void synchronize(void) {
  parser.panicMode = false;
  while (parser.current.type != TOKEN_EOF) {
    if (parser.previous.type == TOKEN_SEMICOLON)
      return; // just finished a statement; the next one can parse cleanly
    switch (parser.current.type) {
    case TOKEN_PRINT:
      return; // a keyword that starts a statement — resume here
    default:
      break;
    }
    advance();
  }
}

static Node *printStatement(void) {
  int line = parser.previous.line; // the 'print' keyword's line
  Node *value = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after value.");
  return newPrint(value, line);
}

static Node *expressionStatement(void) {
  int line = parser.current.line;
  Node *expr = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  // The wrapper records that this expression's value is to be DISCARDED — the
  // compiler will emit an OP_POP after it. Distinguishing this from `print`
  // here, at parse time, keeps the compiler simple.
  return newExprStmt(expr, line);
}

static Node *statement(void) {
  if (match(TOKEN_PRINT))
    return printStatement();
  return expressionStatement();
}

bool parse(const char *source, Program *out) {
  initLexer(source);
  initProgram(out);
  parser.hadError = false;
  parser.panicMode = false;
  advance(); // prime `current` with the first token

  // The program loop: parse statements until end of file. On a syntax error we
  // synchronize and keep going, collecting as many independent errors as we can
  // in a single run — much friendlier than stopping at the first.
  while (!check(TOKEN_EOF)) {
    Node *stmt = statement();
    writeProgram(out, stmt);
    if (parser.panicMode)
      synchronize();
  }

  return !parser.hadError;
}
