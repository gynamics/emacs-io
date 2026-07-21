#ifndef EMACS_IO_UTIL_H
#define EMACS_IO_UTIL_H

#include "emacs-module.h"
#include "stream.h"
#include <stddef.h>

/* ── emacs_env helpers ────────────────────────────────────────────────── */

emacs_value eintern(emacs_env *env, const char *name);
emacs_value efuncall(emacs_env *env, const char *fname, ptrdiff_t nargs,
                     emacs_value *args);

emacs_value estring(emacs_env *env, const char *s);
char       *estrdup(emacs_env *env, emacs_value val);
ptrdiff_t   estrlen(emacs_env *env, emacs_value val);
bool        estring_copy(emacs_env *env, emacs_value val, char *buf, ptrdiff_t *lenp);
char       *estring_alloc(emacs_env *env, emacs_value val, ptrdiff_t *len_out);

emacs_value eint(emacs_env *env, intmax_t n);
intmax_t    eint_extract(emacs_env *env, emacs_value v);

emacs_value euser_ptr(emacs_env *env, void *ptr, void (*fin)(void *));
void       *euser_ptr_get(emacs_env *env, emacs_value val);

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

/* ── error signaling ──────────────────────────────────────────────────── */

void signal_error(emacs_env *env, const char *msg);
void signal_file_error(emacs_env *env, const char *msg);
void signal_stream_error(emacs_env *env, cl_stream_t *s, const char *msg);
void signal_end_of_file(emacs_env *env, cl_stream_t *s);

/* ── string builder (for accumulating output) ─────────────────────────── */

typedef struct {
  char   *data;
  size_t  len;
  size_t  cap;
} strbuf_t;

/* ── keyword helpers ──────────────────────────────────────────────────── */

bool kw_eq(emacs_env *env, emacs_value kw, const char *name);

strbuf_t *strbuf_create(size_t initial_cap);
void      strbuf_free(strbuf_t *sb);
void      strbuf_append(strbuf_t *sb, const char *s, ptrdiff_t len);
void      strbuf_append_char(strbuf_t *sb, int c);
emacs_value strbuf_to_estring(emacs_env *env, strbuf_t *sb);

#endif /* EMACS_IO_UTIL_H */
