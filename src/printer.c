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
    return eintern(env, "nil");
  }
  intmax_t ch = eint_extract(env, args[0]);
  cl_stream_t *s = get_stream_or_default(env, args, nargs, 1, "*standard-output*");
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
    return eintern(env, "nil");
  }

  cl_stream_t *s = get_stream_or_default(env, args, nargs, 1, "*standard-output*");
  check_open(env, s, "cl-write-string");

  /* check if start/end are present */
  bool has_start = false, has_end = false;
  ptrdiff_t start = 0, end = 0;
  for (ptrdiff_t i = 2; i < nargs - 1; i += 2) {
    if (kw_eq(env, args[i], ":start")) { start = eint_extract(env, args[i + 1]); has_start = true; }
    if (kw_eq(env, args[i], ":end"))   { end = eint_extract(env, args[i + 1]); has_end = true; }
  }

  /* fast path: write full string without intermediate allocation */
  if (!has_start && !has_end) {
    stream_write_string(env, s, args[0]);
    return args[0];
  }

  /* slow path: substring */
  ptrdiff_t slen = 0;
  char *str = estring_alloc(env, args[0], &slen);
  if (!str) { signal_error(env, "Cannot extract string"); return eintern(env, "nil"); }

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
    return eintern(env, "nil");
  }
  Fwrite_string(env, nargs, args, data);

  cl_stream_t *s = get_stream_or_default(env, args, nargs, 1, "*standard-output*");
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
    return eintern(env, "nil");
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
  cl_stream_t *s = get_stream_or_default(env, args, nargs, 0, "*standard-output*");
  check_open(env, s, "cl-terpri");
  unsigned char nl = '\n';
  stream_write(s, &nl, 1);
  return eintern(env, "nil");
}

/* ── cl-fresh-line ────────────────────────────────────────────────────── */

emacs_value Ffresh_line(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                         void *data) {
  (void)data;
  /* TODO: track column; always writes newline for now */
  cl_stream_t *s = get_stream_or_default(env, args, nargs, 0, "*standard-output*");
  check_open(env, s, "cl-fresh-line");
  unsigned char nl = '\n';
  stream_write(s, &nl, 1);
  return eintern(env, "t");
}

/* ── cl-finish-output ─────────────────────────────────────────────────── */

emacs_value Ffinish_output(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                            void *data) {
  (void)data;
  /* (cl-finish-output &optional stream) */
  cl_stream_t *s = get_stream_or_default(env, args, nargs, 0, "*standard-output*");
  check_open(env, s, "cl-finish-output");
  stream_finish(s);
  return eintern(env, "nil");
}

/* ── cl-force-output ──────────────────────────────────────────────────── */

emacs_value Fforce_output(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                           void *data) {
  (void)data;
  /* (cl-force-output &optional stream) */
  cl_stream_t *s = get_stream_or_default(env, args, nargs, 0, "*standard-output*");
  check_open(env, s, "cl-force-output");
  stream_flush(s);
  return eintern(env, "nil");
}

/* ── cl-clear-output ──────────────────────────────────────────────────── */

emacs_value Fclear_output(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                           void *data) {
  (void)data;
  /* (cl-clear-output &optional stream)
     Clear pending output buffer. */
  cl_stream_t *s = get_stream_or_default(env, args, nargs, 0, "*standard-output*");
  if (s && stream_is_open(s)) {
    pthread_mutex_lock(&s->mutex);
    if (s->ops && s->ops->clear_output)
      s->ops->clear_output(s);
    pthread_mutex_unlock(&s->mutex);
  }
  return eintern(env, "nil");
}

/* ── cl-clear-input ───────────────────────────────────────────────────── */

emacs_value Fclear_input(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                          void *data) {
  (void)data;
  /* (cl-clear-input &optional stream)
     Discard pending input buffer. */
  cl_stream_t *s = get_stream_or_default(env, args, nargs, 0, "*standard-input*");
  if (s && stream_is_open(s)) {
    pthread_mutex_lock(&s->mutex);
    s->pushback_head = s->pushback_tail = 0;
    if (s->ops && s->ops->clear_input)
      s->ops->clear_input(s);
    pthread_mutex_unlock(&s->mutex);
  }
  return eintern(env, "nil");
}

/* ── cl-write-sequence ────────────────────────────────────────────────── */

emacs_value Fwrite_sequence(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                             void *data) {
  (void)data;
  /* (cl-write-sequence sequence stream &key start end) */
  if (nargs < 2) {
    signal_error(env, "cl-write-sequence requires at least 2 arguments");
    return eintern(env, "nil");
  }

  emacs_value seq = args[0];
  cl_stream_t *s = euser_ptr_stream_get(env, args[1]);
  check_open(env, s, "cl-write-sequence");

  ptrdiff_t len = env->vec_size(env, seq);
  ptrdiff_t start = 0;
  ptrdiff_t end = len;

  for (ptrdiff_t i = 2; i < nargs; i += 2) {
    if (i + 1 >= nargs) break;
    if (kw_eq(env, args[i], ":start"))
      start = eint_extract(env, args[i + 1]);
    else if (kw_eq(env, args[i], ":end"))
      end = eint_extract(env, args[i + 1]);
  }

  if (start < 0) start = 0;
  if (end > len) end = len;
  ptrdiff_t count = end - start;
  if (count <= 0) return seq;

  unsigned char *buf = malloc((size_t)count);
  if (!buf) { signal_error(env, "Memory allocation failed"); return eintern(env, "nil"); }

  for (ptrdiff_t i = 0; i < count; i++)
    buf[i] = (unsigned char)eint_extract(env, env->vec_get(env, seq, start + i));

  stream_write(s, buf, count);
  free(buf);
  return seq;
}

/* ── cl-write-strings ──────────────────────────────────────────────────── */

emacs_value Fwrite_strings(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                            void *data) {
  (void)data;
  /* (cl-write-strings strings &optional stream)
     Write a list or vector of strings to STREAM.  Returns STRINGS.
     Vector access uses vec_get (direct C API, no module crossing);
     list traversal uses car/cdr (one efuncall crossing per element). */
  if (nargs < 1) {
    signal_error(env, "cl-write-strings requires at least 1 argument");
    return eintern(env, "nil");
  }

  cl_stream_t *s = get_stream_or_default(env, args, nargs, 1, "*standard-output*");
  check_open(env, s, "cl-write-strings");

  if (env->eq(env, env->type_of(env, args[0]), eintern(env, "vector"))) {
    ptrdiff_t len = env->vec_size(env, args[0]);
    for (ptrdiff_t i = 0; i < len; i++) {
      stream_write_string(env, s, env->vec_get(env, args[0], i));
      if (env->non_local_exit_check(env) != emacs_funcall_exit_return) break;
    }
  } else {
    emacs_value tail = args[0];
    while (env->is_not_nil(env, tail)) {
      emacs_value str = efuncall(env, "car", 1, &tail);
      stream_write_string(env, s, str);
      if (env->non_local_exit_check(env) != emacs_funcall_exit_return) break;
      tail = efuncall(env, "cdr", 1, &tail);
    }
  }

  return args[0];
}
