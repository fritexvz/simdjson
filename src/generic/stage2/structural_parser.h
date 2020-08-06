// This file contains the common code every implementation uses for stage2
// It is intended to be included multiple times and compiled multiple times
// We assume the file in which it is include already includes
// "simdjson/stage2.h" (this simplifies amalgation)

#include "generic/stage2/logger.h"
#include "generic/stage2/structural_iterator.h"

namespace { // Make everything here private
namespace SIMDJSON_IMPLEMENTATION {
namespace stage2 {

#define SIMDJSON_TRY(EXPR) { auto _err = (EXPR); if (_err) { return _err; } }

struct structural_parser : structural_iterator {

  template<bool STREAMING, typename T>
  WARN_UNUSED really_inline error_code walk_document(T &visitor) noexcept;

  // For non-streaming, to pass an explicit 0 as next_structural, which enables optimizations
  really_inline structural_parser(dom_parser_implementation &_dom_parser, uint32_t start_structural_index)
    : structural_iterator(_dom_parser, start_structural_index) {
  }

  really_inline void log_value(const char *type) {
    logger::log_line(*this, "", type, "");
  }

  really_inline void log_start_value(const char *type) {
    logger::log_line(*this, "+", type, "");
    if (logger::LOG_ENABLED) { logger::log_depth++; }
  }

  really_inline void log_end_value(const char *type) {
    if (logger::LOG_ENABLED) { logger::log_depth--; }
    logger::log_line(*this, "-", type, "");
  }

  really_inline void log_error(const char *error) {
    logger::log_line(*this, "", "ERROR", error);
  }
}; // struct structural_parser

template<bool STREAMING, typename T>
WARN_UNUSED really_inline error_code structural_parser::walk_document(T &visitor) noexcept {
  logger::log_start();

  const uint8_t *value; // Used to keep a value around between states

  //
  // Start the document
  //
  if (at_end()) { return EMPTY; }
  visitor.start_document(*this);

  //
  // Read first value
  //
  switch (*(value = advance())) {
    case '{': switch (*(value = advance())) {
        case '"': goto object_first_field;
        case '}': visitor.empty_object(*this); goto document_end;
        default: log_error("No key in first object field"); return TAPE_ERROR;
      }
    case '[': {
      // Make sure the outer array is closed before continuing; otherwise, there are ways we could get
      // into memory corruption. See https://github.com/simdjson/simdjson/issues/906
      if (!STREAMING) {
        if (buf[dom_parser.structural_indexes[dom_parser.n_structural_indexes - 1]] != ']') {
          return TAPE_ERROR;
        }
      }
      switch (*(value = advance())) {
        case ']': visitor.empty_array(*this); goto document_end;
        default: goto array_first_value;
      }
    }
    default: SIMDJSON_TRY( visitor.parse_root_primitive(*this, value) ); goto document_end;
  }

//
// Object parser states
//
object_first_field:
  SIMDJSON_TRY( visitor.start_object(*this) );
  visitor.increment_count(*this);
  goto object_field;

object_field:
  SIMDJSON_TRY( visitor.parse_key(*this, value) );
  if (unlikely( advance_char() != ':' )) { log_error("Missing colon after key in object"); return TAPE_ERROR; }
  switch (*(value = advance())) {
    case '{': switch (*(value = advance())) {
        case '"': goto object_first_field;
        case '}': visitor.empty_object(*this); goto object_continue;
        default: log_error("No key in first object field"); return TAPE_ERROR;
      }
    case '[': switch (*(value = advance())) {
        case ']': visitor.empty_array(*this); goto object_continue;
        default: goto array_first_value;
      }
    default: SIMDJSON_TRY( visitor.parse_primitive(*this, value) ); goto object_continue;
  }

object_continue:
  switch (advance_char()) {
    case ',': {
      visitor.increment_count(*this);
      value = advance();
      if (unlikely( *value != '"' )) { log_error("Key string missing at beginning of field in object"); return TAPE_ERROR; }
      goto object_field;
    }
    case '}': visitor.end_object(*this); goto scope_end;
    default: log_error("No comma between object fields"); return TAPE_ERROR;
  }

scope_end:
  {
    auto parent = visitor.end_container(*this);
    if (!parent.in_container(*this)) { goto document_end; }
    if (parent.in_array(*this)) { goto array_continue; }
  }
  goto object_continue;

//
// Array parser states
//
array_first_value:
  SIMDJSON_TRY( visitor.start_array(*this) );
  visitor.increment_count(*this);
  goto array_value;

array_value:
  switch (*value) {
    case '{': switch (*(value = advance())) {
        case '"': goto object_first_field;
        case '}': visitor.empty_object(*this); goto array_continue;
        default: log_error("No key in first object field"); return TAPE_ERROR;
      }
    case '[': switch (*(value = advance())) {
        case ']': visitor.empty_array(*this); goto array_continue;
        default: goto array_first_value;
      }
    default: SIMDJSON_TRY( visitor.parse_primitive(*this, value) );
  }

array_continue:
  switch (advance_char()) {
    case ',': visitor.increment_count(*this); value = advance(); goto array_value;
    case ']': visitor.end_array(*this); goto scope_end;
    default: log_error("Missing comma between array values"); return TAPE_ERROR;
  }

document_end:
  SIMDJSON_TRY( visitor.end_document(*this) );

  // If we didn't make it to the end, it's an error
  if ( !STREAMING && dom_parser.next_structural_index != dom_parser.n_structural_indexes ) {
    logger::log_string("More than one JSON value at the root of the document, or extra characters at the end of the JSON!");
    return TAPE_ERROR;
  }

  return SUCCESS;

} // parse_structurals()

} // namespace stage2
} // namespace SIMDJSON_IMPLEMENTATION
} // unnamed namespace
