#include "emacs-module.h"
#include "stream.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── helpers ──────────────────────────────────────────────────────────── */

/* ── cl-write-char ────────────────────────────────────────────────────── */

emacs_value Fwrite_char(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                         void *data) {
  (void)data;
  /* (cl-write-char char &optional stream) */
  if (nargs < 1) {
    signal_error(env, "cl-write-char requires at least 1 argument");
    return syms.v_nil;
  }
  intmax_t ch = eint_extract(env, args[0]);
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 1, syms.var_sout);
  check_open(env, s, "cl-write-char");

  unsigned char b = (unsigned char)(ch & 0xFF);
  stream_write(s, &b, 1);
  return args[0];
}

/* ── cl-write-string ──────────────────────────────────────────────────── */

emacs_value Fwrite_string(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                           void *data) {
  (void)data;
  /* (cl-write-string string &optional stream &key start end) */
  if (nargs < 1) {
    signal_error(env, "cl-write-string requires at least 1 argument");
    return syms.v_nil;
  }

  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 1, syms.var_sout);
  check_open(env, s, "cl-write-string");

  /* check if start/end are present */
  bool has_start = false, has_end = false;
  ptrdiff_t start = 0, end = 0;
  for (ptrdiff_t i = 2; i < nargs - 1; i += 2) {
    if (kw_eq_cached(env, args[i], syms.k_start)) { start = eint_extract(env, args[i + 1]); has_start = true; }
    if (kw_eq_cached(env, args[i], syms.k_end))   { end = eint_extract(env, args[i + 1]); has_end = true; }
  }

  /* fast path: write full string without intermediate allocation */
  if (!has_start && !has_end) {
    stream_write_string(env, s, args[0]);
    return args[0];
  }

  /* slow path: substring */
  ptrdiff_t slen = 0;
  char *str = estring_alloc(env, args[0], &slen);
  if (!str) { signal_error(env, "Cannot extract string"); return syms.v_nil; }

  if (!has_start) start = 0;
  if (!has_end) end = slen;
  if (start < 0) start = 0;
  if (end > slen) end = slen;
  if (end <= start) { free(str); return args[0]; }

  stream_write(s, str + start, end - start);
  free(str);
  return args[0];
}

/* ── cl-write-line ────────────────────────────────────────────────────── */

emacs_value Fwrite_line(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                         void *data) {
  (void)data;
  /* (cl-write-line string &optional stream &key start end) */
  if (nargs < 1) {
    signal_error(env, "cl-write-line requires at least 1 argument");
    return syms.v_nil;
  }
  Fwrite_string(env, nargs, args, data);

  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 1, syms.var_sout);
  check_open(env, s, "cl-write-line");
  unsigned char nl = '\n';
  stream_write(s, &nl, 1);
  return args[0];
}

/* ── cl-write-byte ────────────────────────────────────────────────────── */

emacs_value Fwrite_byte(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                         void *data) {
  (void)data;
  /* (cl-write-byte byte stream) */
  if (nargs < 2) {
    signal_error(env, "cl-write-byte requires 2 arguments");
    return syms.v_nil;
  }
  intmax_t b = eint_extract(env, args[0]);
  cl_stream_t *s = euser_ptr_stream_get(env, args[1]);
  check_open(env, s, "cl-write-byte");

  unsigned char ub = (unsigned char)(b & 0xFF);
  stream_write(s, &ub, 1);
  return args[0];
}

/* ── cl-terpri ────────────────────────────────────────────────────────── */

emacs_value Fterpri(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                     void *data) {
  (void)data;
  /* (cl-terpri &optional stream) */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sout);
  check_open(env, s, "cl-terpri");
  unsigned char nl = '\n';
  stream_write(s, &nl, 1);
  return syms.v_nil;
}

/* ── cl-fresh-line ────────────────────────────────────────────────────── */

emacs_value Ffresh_line(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                         void *data) {
  (void)data;
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sout);
  check_open(env, s, "cl-fresh-line");

  /* column 0 or last char was newline → already at BOL */
  if (s->column == 0 || s->last_char == '\n')
    return syms.v_nil;

  unsigned char nl = '\n';
  stream_write(s, &nl, 1);
  return syms.v_t;
}

/* ── cl-finish-output ─────────────────────────────────────────────────── */

emacs_value Ffinish_output(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                            void *data) {
  (void)data;
  /* (cl-finish-output &optional stream) */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sout);
  check_open(env, s, "cl-finish-output");
  stream_finish(s);
  return syms.v_nil;
}

/* ── cl-force-output ──────────────────────────────────────────────────── */

emacs_value Fforce_output(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                           void *data) {
  (void)data;
  /* (cl-force-output &optional stream) */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sout);
  check_open(env, s, "cl-force-output");
  stream_flush(s);
  return syms.v_nil;
}

/* ── cl-clear-output ──────────────────────────────────────────────────── */

emacs_value Fclear_output(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                           void *data) {
  (void)data;
  /* (cl-clear-output &optional stream)
     Clear pending output buffer. */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sout);
  if (s && stream_is_open(s)) {
    pthread_mutex_lock(&s->mutex);
    if (s->ops && s->ops->clear_output)
      s->ops->clear_output(s);
    pthread_mutex_unlock(&s->mutex);
  }
  return syms.v_nil;
}

/* ── cl-clear-input ───────────────────────────────────────────────────── */

emacs_value Fclear_input(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                          void *data) {
  (void)data;
  /* (cl-clear-input &optional stream)
     Discard pending input buffer. */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  if (s && stream_is_open(s)) {
    pthread_mutex_lock(&s->mutex);
    s->pushback_head = s->pushback_tail = 0;
    if (s->ops && s->ops->clear_input)
      s->ops->clear_input(s);
    pthread_mutex_unlock(&s->mutex);
  }
  return syms.v_nil;
}

/* ── cl-write-sequence ────────────────────────────────────────────────── */

emacs_value Fwrite_sequence(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                             void *data) {
  (void)data;
  /* (cl-write-sequence sequence stream &key start end) */
  if (nargs < 2) {
    signal_error(env, "cl-write-sequence requires at least 2 arguments");
    return syms.v_nil;
  }

  emacs_value seq = args[0];
  cl_stream_t *s = euser_ptr_stream_get(env, args[1]);
  check_open(env, s, "cl-write-sequence");

  bool is_vec = env->eq(env, env->type_of(env, seq), eintern(env, "vector"));
  bool is_list = !is_vec && env->eq(env, env->type_of(env, seq),
                                     eintern(env, "cons"));
  if (!is_vec && !is_list) {
    signal_error(env, "cl-write-sequence: SEQUENCE must be a vector or list");
    return syms.v_nil;
  }

  ptrdiff_t len = is_vec ? env->vec_size(env, seq) : 1 << 30;
  ptrdiff_t start = 0;
  ptrdiff_t end = len;

  for (ptrdiff_t i = 2; i < nargs; i += 2) {
    if (i + 1 >= nargs) break;
    if (kw_eq_cached(env, args[i], syms.k_start))
      start = eint_extract(env, args[i + 1]);
    else if (kw_eq_cached(env, args[i], syms.k_end))
      end = eint_extract(env, args[i + 1]);
  }

  if (start < 0) start = 0;
  if (is_vec && end > len) end = len;
  ptrdiff_t count = end - start;
  if (count <= 0) return seq;

  if (is_vec) {
    unsigned char *buf = malloc((size_t)count);
    if (!buf) { signal_error(env, "Memory allocation failed"); return syms.v_nil; }

    for (ptrdiff_t i = 0; i < count; i++)
      buf[i] = (unsigned char)eint_extract(env,
                env->vec_get(env, seq, start + i));

    stream_write(s, buf, count);
    free(buf);
    return seq;
  }

  /* list: advance to start, write one element at a time */
  emacs_value cell = seq;
  for (ptrdiff_t i = 0; i < start && env->is_not_nil(env, cell); i++)
    cell = efuncall(env, "cdr", 1, &cell);

  ptrdiff_t i = 0;
  while (i < count && env->is_not_nil(env, cell)) {
    unsigned char b = (unsigned char)eint_extract(env,
                       efuncall(env, "car", 1, &cell));
    stream_write(s, &b, 1);
    cell = efuncall(env, "cdr", 1, &cell);
    i++;
  }
  return seq;
}
