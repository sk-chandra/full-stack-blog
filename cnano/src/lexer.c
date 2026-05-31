#include <stdbool.h>
#include <string.h> // strlen

#include "lexer.h"

// The lexer's entire mutable state. Using a single file-static struct (rather
// than passing a context pointer everywhere) keeps the example readable. The
// cost is that the lexer is not reentrant — fine for a single-threaded CLI.
typedef struct {
  const char *start;   // start of the token currently being scanned
  const char *current; // the character we are about to look at
  int line;            // current line number
} Lexer;

static Lexer lexer;

void initLexer(const char *source) {
  lexer.start = source;
  lexer.current = source;
  lexer.line = 1;
}

static bool isAtEnd(void) { return *lexer.current == '\0'; }

static bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Consume and return the current character.
static char advance(void) {
  lexer.current++;
  return lexer.current[-1];
}

// Look at the current character without consuming it ("lookahead").
static char peek(void) { return *lexer.current; }

// Build a token of `type` spanning from lexer.start to lexer.current.
static Token makeToken(TokenType type) {
  Token token;
  token.type = type;
  token.start = lexer.start;
  token.length = (int)(lexer.current - lexer.start);
  token.line = lexer.line;
  return token;
}

// Build an error token whose "text" is a static human-readable message.
static Token errorToken(const char *message) {
  Token token;
  token.type = TOKEN_ERROR;
  token.start = message;
  token.length = (int)strlen(message);
  token.line = lexer.line;
  return token;
}

// Skip spaces, tabs, carriage returns, newlines. We track newlines so error
// messages can report the right line. (Comments would also be skipped here.)
static void skipWhitespace(void) {
  for (;;) {
    char c = peek();
    switch (c) {
    case ' ':
    case '\r':
    case '\t':
      advance();
      break;
    case '\n':
      lexer.line++;
      advance();
      break;
    default:
      return;
    }
  }
}

// Scan a run of digits into a single NUMBER token. (No decimals yet — integers
// only in this slice. Adding floats later means handling a '.' followed by more
// digits right here.)
static Token number(void) {
  while (isDigit(peek()))
    advance();
  return makeToken(TOKEN_NUMBER);
}

Token scanToken(void) {
  skipWhitespace();
  lexer.start = lexer.current; // the new token begins here

  if (isAtEnd())
    return makeToken(TOKEN_EOF);

  char c = advance();

  if (isDigit(c))
    return number();

  switch (c) {
  case '+':
    return makeToken(TOKEN_PLUS);
  case '-':
    return makeToken(TOKEN_MINUS);
  case '*':
    return makeToken(TOKEN_STAR);
  case '/':
    return makeToken(TOKEN_SLASH);
  case '(':
    return makeToken(TOKEN_LPAREN);
  case ')':
    return makeToken(TOKEN_RPAREN);
  }

  return errorToken("Unexpected character.");
}
