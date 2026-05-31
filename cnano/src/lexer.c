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

// A character that may start or continue an identifier/keyword. cnano keeps it
// simple: ASCII letters and underscore. (Real languages also allow Unicode.)
static bool isAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// Consume and return the current character.
static char advance(void) {
  lexer.current++;
  return lexer.current[-1];
}

// Look at the current character without consuming it ("lookahead").
static char peek(void) { return *lexer.current; }

// Conditionally consume: if the current character equals `expected`, eat it and
// return true. This is how the lexer decides between `!` and `!=`: it reads `!`,
// then `match('=')` tells it whether a `=` follows. One character of lookahead
// is all two-character operators need.
static bool match(char expected) {
  if (isAtEnd())
    return false;
  if (*lexer.current != expected)
    return false;
  lexer.current++;
  return true;
}

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

// Peek at the SECOND-next character without consuming anything. Needed to spot
// `//` (a comment) versus a lone `/` (division) during whitespace skipping.
static char peekNext(void) {
  if (isAtEnd())
    return '\0';
  return lexer.current[1];
}

// Skip spaces, tabs, carriage returns, newlines — and line comments. Comments
// are handled HERE, alongside whitespace, rather than as real tokens, because by
// the time the parser runs they should have vanished entirely: they carry no
// meaning, only documentation. We track newlines so error messages stay on the
// right line.
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
    case '/':
      // A `//` line comment runs to the end of the line (but NOT past the
      // newline, so the line counter still ticks on the next loop iteration).
      // A single `/` is division, so we must look ahead before consuming.
      if (peekNext() == '/') {
        while (peek() != '\n' && !isAtEnd())
          advance();
      } else {
        return; // it's the division operator; let scanToken handle it
      }
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

// Scan a "double-quoted" string. We allow newlines inside (tracking the line
// counter) and stop at the closing quote or end of file. The token's text
// INCLUDES the surrounding quotes; the parser strips them. cnano keeps this
// minimal: no escape sequences yet (\n, \" etc.) — a natural future exercise.
static Token string(void) {
  while (peek() != '"' && !isAtEnd()) {
    if (peek() == '\n')
      lexer.line++;
    advance();
  }
  if (isAtEnd())
    return errorToken("Unterminated string.");
  advance(); // consume the closing quote
  return makeToken(TOKEN_STRING);
}

// Decide whether the just-scanned identifier is actually a reserved keyword.
// We compare the lexeme's length and bytes against each keyword. With only three
// keywords a short if-chain is clearest; real lexers use a small trie or hash to
// stay fast as the keyword set grows. The KEYword *recognition* must happen here
// rather than in the parser, because a keyword and a variable name look
// identical until you check the spelling.
static TokenType identifierType(void) {
  int length = (int)(lexer.current - lexer.start);
  const char *s = lexer.start;
  if (length == 4 && memcmp(s, "true", 4) == 0)
    return TOKEN_TRUE;
  if (length == 5 && memcmp(s, "false", 5) == 0)
    return TOKEN_FALSE;
  if (length == 3 && memcmp(s, "nil", 3) == 0)
    return TOKEN_NIL;
  if (length == 5 && memcmp(s, "print", 5) == 0)
    return TOKEN_PRINT;
  if (length == 3 && memcmp(s, "let", 3) == 0)
    return TOKEN_LET;
  if (length == 2 && memcmp(s, "if", 2) == 0)
    return TOKEN_IF;
  if (length == 4 && memcmp(s, "else", 4) == 0)
    return TOKEN_ELSE;
  if (length == 5 && memcmp(s, "while", 5) == 0)
    return TOKEN_WHILE;
  if (length == 3 && memcmp(s, "for", 3) == 0)
    return TOKEN_FOR;
  if (length == 2 && memcmp(s, "in", 2) == 0)
    return TOKEN_IN;
  if (length == 3 && memcmp(s, "and", 3) == 0)
    return TOKEN_AND;
  if (length == 2 && memcmp(s, "or", 2) == 0)
    return TOKEN_OR;
  if (length == 2 && memcmp(s, "fn", 2) == 0)
    return TOKEN_FN;
  if (length == 6 && memcmp(s, "return", 6) == 0)
    return TOKEN_RETURN;
  if (length == 6 && memcmp(s, "struct", 6) == 0)
    return TOKEN_STRUCT;
  // Not a keyword: it's a user-defined identifier (a variable name).
  return TOKEN_IDENTIFIER;
}

// Scan a maximal run of identifier characters, then classify it as a keyword or
// a plain identifier.
static Token identifier(void) {
  while (isAlpha(peek()) || isDigit(peek()))
    advance();
  return makeToken(identifierType());
}

Token scanToken(void) {
  skipWhitespace();
  lexer.start = lexer.current; // the new token begins here

  if (isAtEnd())
    return makeToken(TOKEN_EOF);

  char c = advance();

  if (isAlpha(c))
    return identifier();
  if (isDigit(c))
    return number();

  switch (c) {
  case '+':
    return makeToken(match('=') ? TOKEN_PLUS_EQUAL : TOKEN_PLUS);
  case '-':
    return makeToken(match('=') ? TOKEN_MINUS_EQUAL : TOKEN_MINUS);
  case '.':
    return makeToken(TOKEN_DOT);
  case '*':
    return makeToken(match('=') ? TOKEN_STAR_EQUAL : TOKEN_STAR);
  case '/':
    return makeToken(match('=') ? TOKEN_SLASH_EQUAL : TOKEN_SLASH);
  case '%':
    return makeToken(match('=') ? TOKEN_PERCENT_EQUAL : TOKEN_PERCENT);
  case '(':
    return makeToken(TOKEN_LPAREN);
  case ')':
    return makeToken(TOKEN_RPAREN);
  case '{':
    return makeToken(TOKEN_LBRACE);
  case '}':
    return makeToken(TOKEN_RBRACE);
  case '[':
    return makeToken(TOKEN_LBRACKET);
  case ']':
    return makeToken(TOKEN_RBRACKET);
  case ',':
    return makeToken(TOKEN_COMMA);
  case ':':
    return makeToken(TOKEN_COLON);
  case '?':
    return makeToken(TOKEN_QUESTION);
  case ';':
    return makeToken(TOKEN_SEMICOLON);
  case '"':
    return string();
  // Operators that may be one or two characters. match('=') peeks ahead.
  case '!':
    return makeToken(match('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
  case '=':
    // '==' is equality; a lone '=' is now assignment.
    return makeToken(match('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
  case '<':
    return makeToken(match('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
  case '>':
    return makeToken(match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
  }

  return errorToken("Unexpected character.");
}
