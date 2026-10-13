#ifndef EMACS_IO_UTIL_H
#define EMACS_IO_UTIL_H

#include "emacs-module.h"
#include "stream.h"
#include "syms.h"
#include "strbuf.h"
#include <stddef.h>

/* ── runtime pointer (set once at init) ───────────────────────────────── */

extern struct emacs_runtime *cl_rt;
emacs_env *cl_env(void);

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

cl_stream_t *euser_ptr_stream_get(emacs_env *env, emacs_value val);
emacs_value  euser_ptr_stream_make(emacs_env *env, cl_stream_t *s);
/* Wrap S as a Lisp stream object.  If S is NULL, signal ERRMSG and
   return nil.  Convenience for the "construct or fail" tails. */
emacs_value  stream_or_error(emacs_env *env, cl_stream_t *s, const char *errmsg);
void check_open(emacs_env *env, cl_stream_t *s, const char *op);

/* ── error signaling ──────────────────────────────────────────────────── */

void signal_error(emacs_env *env, const char *msg);
void signal_end_of_file(emacs_env *env, cl_stream_t *s);

/* Resolve end-of-file for a reader whose EOF-ERROR-P is ARGS[ERR_IDX] and
   EOF-VALUE is ARGS[VAL_IDX]: return EOF-VALUE (or nil) when EOF-ERROR-P is
   supplied and nil, otherwise signal `end-of-file'. */
emacs_value eof_result(emacs_env *env, cl_stream_t *s, ptrdiff_t nargs,
                       emacs_value *args, int err_idx, int val_idx);

/* ── sequence classification ──────────────────────────────────────────── */

typedef enum {
  CL_SEQ_VECTOR,
  CL_SEQ_STRING,
  CL_SEQ_LIST,
  CL_SEQ_INVALID
} cl_seq_kind_t;

/* Classify SEQ as a vector, string or list.  On success *len_out receives
   its length (1<<30 for a list, which is treated as unbounded).  Returns
   CL_SEQ_INVALID for anything else, with *len_out set to 0. */
cl_seq_kind_t seq_classify(emacs_env *env, emacs_value seq, ptrdiff_t *len_out);

#endif /* EMACS_IO_UTIL_H */
