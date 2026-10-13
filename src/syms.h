#ifndef EMACS_IO_SYMS_H
#define EMACS_IO_SYMS_H

#include "emacs-module.h"

/* ── pre-interned symbol table ──────────────────────────────────────────
   Symbols and common values interned once at module load (syms_init) so
   hot paths can compare/call without re-interning by name. */
typedef struct {
  /* keyword symbols */
  emacs_value k_direction, k_if_exists, k_if_does_not_exist;
  emacs_value k_element_type, k_external_format;
  emacs_value k_supersede, k_append, k_overwrite, k_error;
  emacs_value k_start, k_end, k_size, k_stream;
  emacs_value k_escape, k_circle, k_pretty, k_level, k_length, k_gensym;
  emacs_value k_preserve_whitespace, k_abort;

  /* direction argument values (:input, :output, etc.) */
  emacs_value d_input, d_output, d_io, d_probe;

  /* Lisp function names */
  emacs_value fn_car, fn_cdr, fn_list, fn_cons, fn_set;
  emacs_value fn_symbol_value, fn_read_from_string, fn_prin1_to_string;
  emacs_value fn_buffer_substring, fn_buffer_size;
  emacs_value fn_error, fn_signal;

  /* common values */
  emacs_value v_nil, v_t, v_character, v_string;
  emacs_value v_end_of_file;
  emacs_value v_unsigned_byte_8;

  /* standard stream variable names */
  emacs_value var_sin, var_sout, var_serr;
} syms_t;

extern syms_t syms;

void syms_init(emacs_env *env);

#endif /* EMACS_IO_SYMS_H */
