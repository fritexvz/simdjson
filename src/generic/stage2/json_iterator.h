#include "generic/stage2/logger.h"

namespace {
namespace SIMDJSON_IMPLEMENTATION {
namespace stage2 {

class json_iterator {
public:
  const uint8_t* const buf;
  uint32_t *next_structural;
  dom_parser_implementation &dom_parser;
  const uint8_t *value{}; // The current JSON value we advanced to

  template<bool STREAMING, typename T>
  WARN_UNUSED really_inline error_code walk_document(T &visitor) noexcept;

  // Start a structural 
  really_inline json_iterator(dom_parser_implementation &_dom_parser, size_t start_structural_index)
    : buf{_dom_parser.buf},
      next_structural{&_dom_parser.structural_indexes[start_structural_index]},
      dom_parser{_dom_parser} {
  }

  // Get the buffer position of the current structural character
  really_inline uint8_t advance_char() {
    value = &buf[*(next_structural++)];
    return *value;
  }
  really_inline size_t remaining_len() {
    return dom_parser.len - *(next_structural-1);
  }

  really_inline bool at_end() {
    return next_structural == &dom_parser.structural_indexes[dom_parser.n_structural_indexes];
  }
  really_inline bool at_beginning() {
    return next_structural == dom_parser.structural_indexes.get();
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
};

template<bool STREAMING, typename T>
WARN_UNUSED really_inline error_code json_iterator::walk_document(T &visitor) noexcept {
  logger::log_start();

  //
  // Start the document
  //
  if (at_end()) { return EMPTY; }
  SIMDJSON_TRY( visitor.start_document(*this) );

  //
  // Read first value
  //
  switch (advance_char()) {
    case '{':
      switch (advance_char()) {
        case '"': goto object_first_field;
        case '}': SIMDJSON_TRY( visitor.empty_object(*this) ); goto document_end;
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
      switch (advance_char()) {
        case ']': SIMDJSON_TRY( visitor.empty_array(*this) ); goto document_end;
        default: goto array_first_value;
      }
    }
    default: SIMDJSON_TRY( visitor.root_primitive(*this, value) ); goto document_end;
  }

//
// Object parser states
//
object_first_field:
  SIMDJSON_TRY( visitor.start_object(*this) );
  goto object_field;

object_field:
  SIMDJSON_TRY( visitor.key(*this, value) );
  if (unlikely( advance_char() != ':' )) { log_error("Missing colon after key in object"); return TAPE_ERROR; }
  switch (advance_char()) {
    case '{':
      switch (advance_char()) {
        case '"': goto object_first_field;
        case '}': SIMDJSON_TRY( visitor.empty_object(*this) ); goto object_continue;
        default: log_error("No key in first object field"); return TAPE_ERROR;
      }
    case '[':
      switch (advance_char()) {
        case ']': SIMDJSON_TRY( visitor.empty_array(*this) ); goto object_continue;
        default: goto array_first_value;
      }
    default: SIMDJSON_TRY( visitor.primitive(*this, value) ); goto object_continue;
  }

object_continue:
  switch (advance_char()) {
    case ',':
      SIMDJSON_TRY( visitor.next_field(*this) );
      if (unlikely( advance_char() != '"' )) { log_error("Key string missing at beginning of field in object"); return TAPE_ERROR; }
      goto object_field;
    case '}': SIMDJSON_TRY( visitor.end_object(*this) ); goto scope_end;
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
  goto array_value;

array_value:
  switch (*value) {
    case '{':
      switch (advance_char()) {
        case '"': goto object_first_field;
        case '}': SIMDJSON_TRY( visitor.empty_object(*this) ); goto array_continue;
        default: log_error("No key in first object field"); return TAPE_ERROR;
      }
    case '[':
      switch (advance_char()) {
        case ']': SIMDJSON_TRY( visitor.empty_array(*this) ); goto array_continue;
        default: goto array_first_value;
      }
    default: SIMDJSON_TRY( visitor.primitive(*this, value) ); goto array_continue;
  }

array_continue:
  switch (advance_char()) {
    case ',': SIMDJSON_TRY( visitor.next_array_element(*this) ); advance_char(); goto array_value;
    case ']': SIMDJSON_TRY( visitor.end_array(*this) ); goto scope_end;
    default: log_error("Missing comma between array values"); return TAPE_ERROR;
  }

document_end:
  SIMDJSON_TRY( visitor.end_document(*this) );

  // If we didn't make it to the end, it's an error
  if ( !STREAMING && dom_parser.next_structural_index != dom_parser.n_structural_indexes ) {
    log_error("More than one JSON value at the root of the document, or extra characters at the end of the JSON!");
    return TAPE_ERROR;
  }

  return SUCCESS;

} // parse_structurals()

} // namespace stage2
} // namespace SIMDJSON_IMPLEMENTATION
} // unnamed namespace
