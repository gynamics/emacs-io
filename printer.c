#include "emacs-module.h"
#include "stream.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern cl_stream_t *get_stream_or_default(emacs_env *env, emacs_value *args,
                                           ptrdiff_t nargs, int argidx,
                                           const char *var);

/* ── helpers ──────────────────────────────────────────────────────────── */

static cl_stream_t *get_output_stream(emacs_env *env, emacs_value *args,
                                       ptrdiff_t nargs, int argidx) {
  if (argidx < nargs && env->is_not_nil(env, args[argidx]))
    return euser_ptr_stream_get(env, args[argidx]);
  emacs_value sv = efuncall(env, "symbol-value", 1,
                             (emacs_value[]){ eintern(env, "*standard-output*") });
  return euser_ptr_stream_get(env, sv);
}

static void write_check(emacs_env *env, cl_stream_t *s, const char *op) {
  if (!s || !stream_is_open(s)) {
    char buf[256];
    snprintf(buf, sizeof(buf), "Stream closed in %s", op);
    signal_stream_error(env, s, buf);
  }
}

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
  cl_stream_t *s = get_output_stream(env, args, nargs, 1);
  write_check(env, s, "cl-write-char");

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

  cl_stream_t *s = get_output_stream(env, args, nargs, 1);

  ptrdiff_t slen = 0;
  char *str = estring_alloc(env, args[0], &slen);
  if (!str) { signal_error(env, "Cannot extract string"); return eintern(env, "nil"); }

  ptrdiff_t start = 0;
  ptrdiff_t end = slen;

  /* parse &key start end */
  for (ptrdiff_t i = 2; i < nargs; i += 2) {
    if (i + 1 >= nargs) break;
    if (kw_eq(env, args[i], ":start"))
      start = eint_extract(env, args[i + 1]);
    else if (kw_eq(env, args[i], ":end"))
      end = eint_extract(env, args[i + 1]);
  }

  if (start < 0) start = 0;
  if (end > slen) end = slen;
  if (end <= start) { free(str); return args[0]; }

  write_check(env, s, "cl-write-string");
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

  cl_stream_t *s = get_output_stream(env, args, nargs, 1);
  write_check(env, s, "cl-write-line");
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
  write_check(env, s, "cl-write-byte");

  unsigned char ub = (unsigned char)(b & 0xFF);
  stream_write(s, &ub, 1);
  return args[0];
}

/* ── cl-terpri ────────────────────────────────────────────────────────── */

emacs_value Fterpri(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                     void *data) {
  (void)data;
  /* (cl-terpri &optional stream) */
  cl_stream_t *s = get_output_stream(env, args, nargs, 0);
  write_check(env, s, "cl-terpri");
  unsigned char nl = '\n';
  stream_write(s, &nl, 1);
  return eintern(env, "nil");
}

/* ── cl-fresh-line ────────────────────────────────────────────────────── */

emacs_value Ffresh_line(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                         void *data) {
  (void)data;
  /* (cl-fresh-line &optional stream)
     Write a newline if not at start of line. Since we don't track columns
     in core streams, always write a newline. */
  cl_stream_t *s = get_output_stream(env, args, nargs, 0);
  write_check(env, s, "cl-fresh-line");
  unsigned char nl = '\n';
  stream_write(s, &nl, 1);
  return eintern(env, "t");
}

/* ── cl-finish-output ─────────────────────────────────────────────────── */

emacs_value Ffinish_output(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                            void *data) {
  (void)data;
  /* (cl-finish-output &optional stream) */
  cl_stream_t *s = get_output_stream(env, args, nargs, 0);
  write_check(env, s, "cl-finish-output");
  stream_finish(s);
  return eintern(env, "nil");
}

/* ── cl-force-output ──────────────────────────────────────────────────── */

emacs_value Fforce_output(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                           void *data) {
  (void)data;
  /* (cl-force-output &optional stream) */
  cl_stream_t *s = get_output_stream(env, args, nargs, 0);
  write_check(env, s, "cl-force-output");
  stream_flush(s);
  return eintern(env, "nil");
}

/* ── cl-clear-output ──────────────────────────────────────────────────── */

emacs_value Fclear_output(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                           void *data) {
  (void)data;
  /* (cl-clear-output &optional stream)
     Clear pending output buffer. For fd streams, discard write buffer. */
  cl_stream_t *s = get_output_stream(env, args, nargs, 0);
  if (s && stream_is_open(s) && s->impl) {
    fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
    fi->wlen = 0;
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
  if (s && stream_is_open(s) && s->impl) {
    fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
    fi->rpos = 0;
    fi->rlen = 0;
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
  write_check(env, s, "cl-write-sequence");

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
