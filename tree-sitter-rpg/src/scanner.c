// External scanner for the tree-sitter RPG grammar.
//
// IBM ILE RPG source is column-sensitive. Traditional ("fixed-form") lines
// carry a specification-type letter in column 6 (0-indexed 5), with the first
// five columns being the sequence-number area. Free-form code either lives in
// columns 8-80 of an otherwise fixed-form member, or follows a `**FREE`
// directive that switches the whole member to free format.
//
// Design: graceful degradation (see docs/adr/0001). In fixed mode this scanner
// gives EVERY line a total classification, in priority order:
//   newline -> `**` directive -> fixed spec (H/F/D/C/P) -> fixed comment ->
//   `/COPY`/`/INCLUDE` directive -> else `unknown_line` (whole line consumed).
// Because every fixed-mode line becomes exactly one token that ends at the
// newline, a single unmodelled line can never cascade into the rest of the
// file. The parser resyncs at every line boundary.
//
// The sequence-number area (columns 1-5) may legitimately contain letters in
// real IBM-i exports (labels such as `CPY` on /COPY lines, or `B01`/`X01`/`E01`
// on free-form calc). We therefore do NOT reject letters there; classification
// keys on column 6 (the spec/comment/directive column) instead. Anything we do
// not recognise falls through to `unknown_line`.
//
// After `**FREE` (or any `**` directive) the scanner enters free mode and stops
// producing fixed-form / unknown_line tokens, letting the free-form grammar
// parse the member.

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum TokenType {
  FIXED_H_LINE,
  FIXED_F_LINE,
  FIXED_D_LINE,
  FIXED_C_LINE,
  FIXED_P_LINE,
  FIXED_COMMENT_LINE,
  COPY_DIRECTIVE,
  FREE_DIRECTIVE,
  UNKNOWN_LINE,
  NEWLINE,
};

typedef struct {
  // Once a `**FREE` (or other `**`) directive is seen, the member is treated as
  // free-form and column-based detection is switched off.
  bool free_mode;
} Scanner;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline bool is_line_end(int32_t c) { return c == '\n' || c == '\r'; }

// Consume characters until (but not including) the end of the current line.
static void consume_to_line_end(TSLexer *lexer) {
  while (!lexer->eof(lexer) && !is_line_end(lexer->lookahead)) {
    advance(lexer);
  }
}

// Emit the remainder of the current line as UNKNOWN_LINE. Assumes at least one
// character has already been consumed since scanning started (so the token is
// never zero-width). Returns false if UNKNOWN_LINE is not currently valid.
static bool emit_unknown(TSLexer *lexer, const bool *valid_symbols) {
  if (!valid_symbols[UNKNOWN_LINE]) {
    return false;
  }
  consume_to_line_end(lexer);
  lexer->result_symbol = UNKNOWN_LINE;
  lexer->mark_end(lexer);
  return true;
}

// Positioned on `/`. Consume the directive and the rest of the line, then emit
// COPY_DIRECTIVE for `/COPY` and `/INCLUDE` (case-insensitive) or UNKNOWN_LINE
// for any other `/` directive.
static bool emit_directive(TSLexer *lexer, const bool *valid_symbols) {
  advance(lexer);  // consume '/'
  char word[9];
  int n = 0;
  while (n < 8) {
    int32_t c = lexer->lookahead;
    if (c >= 'A' && c <= 'Z') {
      c = c - 'A' + 'a';
    }
    if (c >= 'a' && c <= 'z') {
      word[n++] = (char)c;
      advance(lexer);
    } else {
      break;
    }
  }
  word[n] = '\0';
  bool is_copy = (strcmp(word, "copy") == 0) || (strcmp(word, "include") == 0);
  consume_to_line_end(lexer);
  if (is_copy && valid_symbols[COPY_DIRECTIVE]) {
    lexer->result_symbol = COPY_DIRECTIVE;
  } else if (valid_symbols[UNKNOWN_LINE]) {
    lexer->result_symbol = UNKNOWN_LINE;
  } else {
    return false;
  }
  lexer->mark_end(lexer);
  return true;
}

void *tree_sitter_rpg_external_scanner_create(void) {
  Scanner *scanner = (Scanner *)calloc(1, sizeof(Scanner));
  return scanner;
}

void tree_sitter_rpg_external_scanner_destroy(void *payload) {
  free(payload);
}

unsigned tree_sitter_rpg_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *scanner = (Scanner *)payload;
  buffer[0] = scanner->free_mode ? 1 : 0;
  return 1;
}

void tree_sitter_rpg_external_scanner_deserialize(void *payload, const char *buffer,
                                                  unsigned length) {
  Scanner *scanner = (Scanner *)payload;
  scanner->free_mode = (length > 0) ? (buffer[0] != 0) : false;
}

bool tree_sitter_rpg_external_scanner_scan(void *payload, TSLexer *lexer,
                                           const bool *valid_symbols) {
  Scanner *scanner = (Scanner *)payload;

  // 1. Newlines. Handled first so they win regardless of column state.
  if (valid_symbols[NEWLINE] && is_line_end(lexer->lookahead)) {
    if (lexer->lookahead == '\r') {
      advance(lexer);
    }
    if (lexer->lookahead == '\n') {
      advance(lexer);
    }
    lexer->result_symbol = NEWLINE;
    return true;
  }

  // Everything below is line-oriented: only meaningful at the start of a line.
  if (lexer->get_column(lexer) != 0) {
    return false;
  }

  // 2. `**` directives (e.g. **FREE, **CTDATA). Valid in either mode; the
  //    first one flips the scanner into free format.
  if (lexer->lookahead == '*') {
    advance(lexer);
    if (lexer->lookahead == '*') {
      if (!valid_symbols[FREE_DIRECTIVE]) {
        return false;
      }
      consume_to_line_end(lexer);
      lexer->result_symbol = FREE_DIRECTIVE;
      lexer->mark_end(lexer);
      scanner->free_mode = true;
      return true;
    }
    // A lone `*` in column 1 is not a fixed-form comment (those live in
    // column 6/7). In free mode let the grammar lex it (e.g. `*inlr`); in fixed
    // mode absorb the whole line so it cannot derail the parse.
    if (scanner->free_mode) {
      return false;
    }
    return emit_unknown(lexer, valid_symbols);
  }

  // In free mode, no column-based fixed-form detection.
  if (scanner->free_mode) {
    return false;
  }

  // Nothing to classify at end of input.
  if (lexer->eof(lexer)) {
    return false;
  }

  // 3. Consume the sequence-number area (columns 1-5, idx 0-4). Letters are
  //    allowed here (real exports carry labels); classification keys on the
  //    spec column, not on this area.
  for (int i = 0; i < 5; i++) {
    if (lexer->eof(lexer) || is_line_end(lexer->lookahead)) {
      // A line shorter than six columns. If we consumed nothing, let the
      // newline machinery handle it; otherwise absorb the short line.
      if (i == 0) {
        return false;
      }
      return emit_unknown(lexer, valid_symbols);
    }
    advance(lexer);
  }

  // 4. Column 6 (idx 5): the spec / comment / directive column.
  int32_t spec = lexer->lookahead;
  enum TokenType type;
  bool is_spec = true;
  switch (spec) {
    case 'H':
    case 'h':
      type = FIXED_H_LINE;
      break;
    case 'F':
    case 'f':
      type = FIXED_F_LINE;
      break;
    case 'D':
    case 'd':
      type = FIXED_D_LINE;
      break;
    case 'C':
    case 'c':
      type = FIXED_C_LINE;
      break;
    case 'P':
    case 'p':
      type = FIXED_P_LINE;
      break;
    default:
      is_spec = false;
      break;
  }

  if (is_spec) {
    if (!valid_symbols[type]) {
      return emit_unknown(lexer, valid_symbols);
    }
    advance(lexer);  // consume the spec letter
    consume_to_line_end(lexer);
    lexer->result_symbol = type;
    lexer->mark_end(lexer);
    return true;
  }

  // Fixed-form full-line comment: `*` in the spec column (idx 5).
  if (spec == '*') {
    if (valid_symbols[FIXED_COMMENT_LINE]) {
      consume_to_line_end(lexer);
      lexer->result_symbol = FIXED_COMMENT_LINE;
      lexer->mark_end(lexer);
      return true;
    }
    return emit_unknown(lexer, valid_symbols);
  }

  // `/COPY` / `/INCLUDE` (or other `/` directive) with `/` in the spec column.
  if (spec == '/') {
    return emit_directive(lexer, valid_symbols);
  }

  if (spec == ' ') {
    advance(lexer);  // move to column 7 (idx 6)
    // Classic comment: `*` in column 7.
    if (lexer->lookahead == '*') {
      if (valid_symbols[FIXED_COMMENT_LINE]) {
        consume_to_line_end(lexer);
        lexer->result_symbol = FIXED_COMMENT_LINE;
        lexer->mark_end(lexer);
        return true;
      }
      return emit_unknown(lexer, valid_symbols);
    }
    // `/COPY` / `/INCLUDE` with `/` in column 7.
    if (lexer->lookahead == '/') {
      return emit_directive(lexer, valid_symbols);
    }
    // Free-form calc or anything else in a fixed member: opaque.
    return emit_unknown(lexer, valid_symbols);
  }

  // Any other character in the spec column: opaque line.
  return emit_unknown(lexer, valid_symbols);
}
