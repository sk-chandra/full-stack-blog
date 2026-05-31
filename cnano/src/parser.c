#include <stdio.h>
#include <stdlib.h>

#include "ast.h"
#include "lexer.h"
#include "parser.h"

// The grammar we implement, written in EBNF. Each rule below becomes a function.
// Lower rules bind tighter (higher precedence):
//
//   expression -> term ;
//   term       -> factor ( ( "+" | "-" ) factor )* ;   // left-associative
//   factor     -> unary  ( ( "*" | "/" ) unary  )* ;   // left-associative
//   unary      -> "-" unary | primary ;
//   primary    -> NUMBER | "(" expression ")" ;
//
// Precedence falls out of the call chain: term calls factor calls unary calls
// primary. Because factor is "below" term, multiplication is grouped before
// addition automatically. Associativity falls out of the loops (`while`): we
// fold left-to-right, so 1-2-3 parses as (1-2)-3, which is what we want.

typedef struct {
  Token current;  // the next token to consume (one-token lookahead)
  Token previous; // the most recently consumed token
  bool hadError;
} Parser;

static Parser parser;

// --- error reporting -------------------------------------------------------

static void errorAt(Token *token, const char *message) {
  // Only report the first error per parse; after an error the parser state is
  // unreliable and further messages tend to be noise.
  if (parser.hadError)
    return;
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
static Node *term(void);
static Node *factor(void);
static Node *unary(void);
static Node *primary(void);

static Node *expression(void) { return term(); }

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
  if (match(TOKEN_MINUS)) {
    int line = parser.previous.line;
    // Recursing into unary() (not primary) makes `--5` parse as -(-5).
    Node *operand = unary();
    return newUnary(OP_NODE_NEGATE, operand, line);
  }
  return primary();
}

static Node *primary(void) {
  if (match(TOKEN_NUMBER)) {
    // strtoll parses the slice of source text the token points at. The token is
    // not NUL-terminated on its own, but it is followed by more source (or the
    // final '\0'), and strtoll stops at the first non-digit, so this is safe.
    Value value = (Value)strtoll(parser.previous.start, NULL, 10);
    return newNumber(value, parser.previous.line);
  }
  if (match(TOKEN_LPAREN)) {
    Node *node = expression();
    consume(TOKEN_RPAREN, "Expect ')' after expression.");
    return node;
  }
  errorAt(&parser.current, "Expect a number or '('.");
  return NULL;
}

Node *parse(const char *source) {
  initLexer(source);
  parser.hadError = false;
  advance(); // prime `current` with the first token

  Node *tree = expression();
  consume(TOKEN_EOF, "Expect end of expression.");

  if (parser.hadError) {
    freeNode(tree); // freeNode tolerates NULL and partial trees
    return NULL;
  }
  return tree;
}
