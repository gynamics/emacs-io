#ifndef EMACS_IO_UTIL_H
#define EMACS_IO_UTIL_H

#include "emacs-module.h"
#include "stream.h"
#include <stddef.h>

/* ── runtime pointer (set once at init) ───────────────────────────────── */

extern struct emacs_runtime *cl_rt;
emacs_env *cl_env(void);

/* ── pre-interned symbol table ────────────────────────────────────────── */

typedef struct {
  /* keyword symbols */
  emacs_value k_direction, k_if_exists, k_if_does_not_exist;
  emacs_value k_element_type, k_external_format;
  emacs_value k_supersede, k_append, k_overwrite, k_error;
  emacs_value k_start, k_end, k_size, k_stream;
  emacs_value k_escape, k_radix, k_base, k_circle, k_pretty, k_level, k_length;
  emacs_value k_case, k_gensym, k_array, k_readably;
  emacs_value k_right_margin, k_miser_width, k_pprint_dispatch, k_lines;
  emacs_value k_preserve_whitespace, k_abort;

  /* direction argument values (:input, :output, etc.) */
  emacs_value d_input, d_output, d_io, d_probe;

  /* Lisp function names */
  emacs_value fn_car, fn_cdr, fn_list, fn_cons, fn_concat, fn_set;
  emacs_value fn_symbol_value, fn_read_from_string, fn_prin1_to_string;
  emacs_value fn_buffer_substring, fn_buffer_size;
  emacs_value fn_defalias, fn_provide, fn_error;

  /* common values */
  emacs_value v_nil, v_t, v_character, v_string;
  emacs_value v_end_of_file;
  emacs_value v_unsigned_byte_8;

  /* standard stream variable names */
  emacs_value var_sin, var_sout, var_serr;
} syms_t;

extern syms_t syms;

void syms_init(emacs_env *env);

/* ── cached API alternatives (avoid eintern on every call) ────────────── */

bool kw_eq_cached(emacs_env *env, emacs_value kw, emacs_value sym);
emacs_value efuncall_cached(emacs_env *env, emacs_value sym, ptrdiff_t nargs,
                            emacs_value *args);
cl_stream_t *get_stream_or_default_cached(emacs_env *env, emacs_value *args,
                                           ptrdiff_t nargs, int argidx,
                                           emacs_value var_sym);

/* ── emacs_env helpers ────────────────────────────────────────────────── */

emacs_value eintern(emacs_env *env, const char *name);
emacs_value efuncall(emacs_env *env, const char *fname, ptrdiff_t nargs,
                     emacs_value *args);
emacs_value estring(emacs_env *env, const char *s);
char       *estrdup(emacs_env *env, emacs_value val);
bool        estring_copy(emacs_env *env, emacs_value val, char *buf, ptrdiff_t *lenp);
char       *estring_alloc(emacs_env *env, emacs_value val, ptrdiff_t *len_out);
emacs_value eint(emacs_env *env, intmax_t n);
intmax_t    eint_extract(emacs_env *env, emacs_value v);

/* ── defun registration ───────────────────────────────────────────────── */

typedef emacs_value (*emacs_subr_fn)(emacs_env *env, ptrdiff_t nargs,
                                     emacs_value *args, void *data);

void defun(emacs_env *env, const char *name, emacs_subr_fn fn,
           ptrdiff_t min, ptrdiff_t max, const char *doc, void *data);
void provide(emacs_env *env, const char *feature);

/* ── stream <-> emacs_value helpers ────────────────────────────────────── */

#define STREAM_USER_PTR_TAG "emacs-io-cl-stream"

cl_stream_t *euser_ptr_stream_get(emacs_env *env, emacs_value val);
emacs_value  euser_ptr_stream_make(emacs_env *env, cl_stream_t *s);
cl_stream_t *get_stream_or_default(emacs_env *env, emacs_value *args,
                                    ptrdiff_t nargs, int argidx,
                                    const char *var);
void check_open(emacs_env *env, cl_stream_t *s, const char *op);

/* ── error signaling ──────────────────────────────────────────────────── */

void signal_error(emacs_env *env, const char *msg);

/* ── keyword helpers ──────────────────────────────────────────────────── */

bool kw_eq(emacs_env *env, emacs_value kw, const char *name);

/* ── string builder ───────────────────────────────────────────────────── */

typedef struct {
  char   *data;
  size_t  len;
  size_t  cap;
} strbuf_t;

strbuf_t *strbuf_create(size_t initial_cap);
void      strbuf_free(strbuf_t *sb);
bool      strbuf_append(strbuf_t *sb, const char *s, ptrdiff_t len);
emacs_value strbuf_to_estring(emacs_env *env, strbuf_t *sb);

#endif /* EMACS_IO_UTIL_H */
