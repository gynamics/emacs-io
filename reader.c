#include "emacs-module.h"
#include "stream.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── helpers ──────────────────────────────────────────────────────────── */

cl_stream_t *get_stream_or_default(emacs_env *env, emacs_value *args,
                                    ptrdiff_t nargs, int argidx,
                                    const char *var) {
  if (argidx < nargs && env->is_not_nil(env, args[argidx]))
    return euser_ptr_stream_get(env, args[argidx]);

  emacs_value sv = efuncall(env, "symbol-value", 1,
                             (emacs_value[]){ eintern(env, var) });
  return euser_ptr_stream_get(env, sv);
}

static void check_open(emacs_env *env, cl_stream_t *s, const char *op) {
  if (!s || !stream_is_open(s)) {
    char buf[256];
    snprintf(buf, sizeof(buf), "Stream is closed in %s", op);
    signal_stream_error(env, s, buf);
  }
}

/* ── cl-read-char ─────────────────────────────────────────────────────── */

emacs_value Fread_char(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                        void *data) {
  (void)data;
  /* (cl-read-char &optional stream eof-error-p eof-value) */
  cl_stream_t *s = get_stream_or_default(env, args, nargs, 0, "*standard-input*");
  check_open(env, s, "cl-read-char");

  unsigned char ch;
  ptrdiff_t r = stream_read(s, &ch, 1);
  if (r <= 0) {
    if (nargs >= 2 && !env->is_not_nil(env, args[1])) /* eof-error-p nil */
      return (nargs >= 3) ? args[2] : eintern(env, "nil");
    signal_end_of_file(env, s);
    return eintern(env, "nil");
  }
  return eint(env, ch);
}

/* ── cl-read-line ─────────────────────────────────────────────────────── */

emacs_value Fread_line(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                       void *data) {
  (void)data;
  /* (cl-read-line &optional stream eof-error-p eof-value) */
  cl_stream_t *s = get_stream_or_default(env, args, nargs, 0, "*standard-input*");
  check_open(env, s, "cl-read-line");

  strbuf_t *sb = strbuf_create(4096);
  if (!sb) { signal_error(env, "Memory allocation failed"); return eintern(env, "nil"); }

  unsigned char ch;
  bool saw_newline = false;

  for (;;) {
    ptrdiff_t r = stream_read(s, &ch, 1);
    if (r <= 0) break;
    if (ch == '\n') { saw_newline = true; break; }
    if (ch == '\r') {
      /* peek ahead for \n */
      unsigned char next;
      ptrdiff_t r2 = stream_read(s, &next, 1);
      if (r2 > 0 && next == '\n') { saw_newline = true; break; }
      /* put back \r since we already consumed it — approximate */
      strbuf_append_char(sb, '\r');
      if (r2 > 0) { /* push the next char back */ }
      saw_newline = true;
      break;
    }
    strbuf_append_char(sb, ch);
  }

  if (sb->len == 0 && !saw_newline) {
    strbuf_free(sb);
    if (nargs >= 2 && !env->is_not_nil(env, args[1]))
      return (nargs >= 3) ? args[2] : eintern(env, "nil");
    signal_end_of_file(env, s);
    return eintern(env, "nil");
  }

  emacs_value line = strbuf_to_estring(env, sb);
  strbuf_free(sb);

  /* return (values line missing-newline-p) as a list */
  emacs_value cons_args[2];
  cons_args[0] = line;
  cons_args[1] = saw_newline ? eintern(env, "nil") : eintern(env, "t");
  return efuncall(env, "list", 2, cons_args);
}

/* ── cl-read-byte ─────────────────────────────────────────────────────── */

emacs_value Fread_byte(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                       void *data) {
  (void)data;
  /* (cl-read-byte stream &optional eof-error-p eof-value) */
  cl_stream_t *s = get_stream_or_default(env, args, nargs, 0, "*standard-input*");
  check_open(env, s, "cl-read-byte");

  unsigned char b;
  ptrdiff_t r = stream_read(s, &b, 1);
  if (r <= 0) {
    if (nargs >= 2 && !env->is_not_nil(env, args[1]))
      return (nargs >= 3) ? args[2] : eintern(env, "nil");
    signal_end_of_file(env, s);
    return eintern(env, "nil");
  }
  return eint(env, b);
}

/* ── cl-peek-char ──────────────────────────────────────────────────────── */

emacs_value Fpeek_char(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                       void *data) {
  (void)data;
  /* (cl-peek-char &optional peek-type stream eof-error-p eof-value) */
  /* Simplified: just peek one character. */
  cl_stream_t *s = get_stream_or_default(env, args, nargs,
                                          (nargs >= 1) ? 1 : 0,
                                          "*standard-input*");
  check_open(env, s, "cl-peek-char");

  /* hack: read a char, store it back in the stream's pushback-like buffer?
     Since our core stream doesn't have pushback, we approximate:
     For fd streams: use fds_listen to check if data available.
     For string streams: just read+restore position.
  */
  if (!stream_listen(s)) {
    return eintern(env, "nil");
  }

  /* For simplicity: read and we'll lose it. A full pushback would require
     stream-level unread support. */
  unsigned char ch;
  ptrdiff_t r = stream_read(s, &ch, 1);
  if (r <= 0) {
    return eintern(env, "nil");
  }
  /* We consumed the char — in a real pushback, we'd push it back.
     For now, just return the value. */
  return eint(env, ch);
}

/* ── cl-unread-char ────────────────────────────────────────────────────── */

emacs_value Funread_char(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                         void *data) {
  (void)data;
  (void)nargs;
  (void)args;
  /* (cl-unread-char char &optional stream) */
  /* Our core stream lacks pushback; signal an error for now. */
  signal_error(env, "cl-unread-char: pushback not implemented on core stream");
  return eintern(env, "nil");
}

/* ── cl-listen ────────────────────────────────────────────────────────── */

emacs_value Flisten(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                    void *data) {
  (void)data;
  /* (cl-listen &optional stream) */
  cl_stream_t *s = get_stream_or_default(env, args, nargs, 0, "*standard-input*");
  if (!s || !stream_is_open(s)) return eintern(env, "nil");

  return stream_listen(s) ? eintern(env, "t") : eintern(env, "nil");
}

/* ── cl-read-sequence ──────────────────────────────────────────────────── */

emacs_value Fread_sequence(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                           void *data) {
  (void)data;
  /* (cl-read-sequence sequence stream &key start end) */
  if (nargs < 2) {
    signal_error(env, "cl-read-sequence requires at least 2 arguments");
    return eintern(env, "nil");
  }

  emacs_value seq = args[0];
  cl_stream_t *s = euser_ptr_stream_get(env, args[1]);
  if (!s || !stream_is_open(s))
    signal_stream_error(env, s, "Stream is closed");

  ptrdiff_t len = env->vec_size(env, seq);
  ptrdiff_t start = 0;
  ptrdiff_t end = len;

  /* parse &key start end */
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
  if (count <= 0) return eint(env, start);

  /* read a chunk and set vector elements */
  unsigned char *buf = malloc((size_t)count);
  if (!buf) { signal_error(env, "Memory allocation failed"); return eintern(env, "nil"); }

  ptrdiff_t r = stream_read(s, buf, count);
  for (ptrdiff_t i = 0; i < r && i < count; i++)
    env->vec_set(env, seq, start + i, eint(env, buf[i]));

  free(buf);
  return eint(env, start + (r > 0 ? r : 0));
}
