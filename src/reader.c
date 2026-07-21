#include "emacs-module.h"
#include "stream.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── helpers ──────────────────────────────────────────────────────────── */

void check_open(emacs_env *env, cl_stream_t *s, const char *op) {
  if (!s || !stream_is_open(s)) {
    char buf[256];
    snprintf(buf, sizeof(buf), "Stream is closed in %s", op);
    signal_error(env, buf);
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
    signal_error(env, "End of file on stream");
    return eintern(env, "nil");
  }
  return eint(env, ch);
}

/* ── cl-read-line ─────────────────────────────────────────────────────── */

#define READ_LINE_CHUNK 4096

emacs_value Fread_line(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                       void *data) {
  (void)data;
  cl_stream_t *s = get_stream_or_default(env, args, nargs, 0, "*standard-input*");
  check_open(env, s, "cl-read-line");

  strbuf_t *sb = strbuf_create(4096);
  if (!sb) { signal_error(env, "Memory allocation failed"); return eintern(env, "nil"); }

  unsigned char chunk[READ_LINE_CHUNK];
  bool saw_newline = false;

  for (;;) {
    ptrdiff_t r = stream_read(s, chunk, sizeof(chunk));
    if (r <= 0) break;

    ptrdiff_t nl_off = -1;
    ptrdiff_t skip = 1;  /* bytes to skip for newline (1 for \n, 2 for \r\n) */

    for (ptrdiff_t i = 0; i < r; i++) {
      if (chunk[i] == '\n') { nl_off = i; skip = 1; break; }
      if (chunk[i] == '\r') {
        if (i + 1 < r && chunk[i + 1] == '\n')
          { nl_off = i; skip = 2; break; }
        nl_off = i; skip = 1; break;
      }
    }

    if (nl_off >= 0) {
      strbuf_append(sb, (const char *)chunk, nl_off);
      /* push leftover in forward order (FIFO queue: first in → first out) */
      for (ptrdiff_t i = nl_off + skip; i < r; i++)
        stream_unread(s, chunk[i]);
      saw_newline = true;
      break;
    }

    /* no newline found — keep accumulating */
    strbuf_append(sb, (const char *)chunk, r);
  }

  if (sb->len == 0 && !saw_newline) {
    strbuf_free(sb);
    if (nargs >= 2 && !env->is_not_nil(env, args[1]))
      return (nargs >= 3) ? args[2] : eintern(env, "nil");
    signal_error(env, "End of file on stream");
    return eintern(env, "nil");
  }

  emacs_value line = strbuf_to_estring(env, sb);
  strbuf_free(sb);

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
    signal_error(env, "End of file on stream");
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

  if (!stream_listen(s)) {
    return eintern(env, "nil");
  }

  /* read one char, push it back via stream_unread */
  unsigned char ch;
  ptrdiff_t r = stream_read(s, &ch, 1);
  if (r <= 0) {
    return eintern(env, "nil");
  }
  /* push it back */
  stream_unread(s, ch);
  return eint(env, ch);
}

/* ── cl-unread-char ────────────────────────────────────────────────────── */

emacs_value Funread_char(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                         void *data) {
  (void)data;
  (void)nargs;
  (void)args;
  if (nargs < 1) { signal_error(env, "cl-unread-char requires at least 1 argument"); return eintern(env, "nil"); }
  cl_stream_t *s = get_stream_or_default(env, args, nargs,
                                          (nargs >= 2) ? 1 : 0,
                                          "*standard-input*");
  intmax_t ch = eint_extract(env, args[0]);
  stream_unread(s, (int)ch);
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
    signal_error(env, "Stream is closed");

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

/* ── cl-read-string ────────────────────────────────────────────────────── */

emacs_value Fread_string(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                          void *data) {
  (void)data;
  /* (cl-read-string n stream &optional eof-error-p eof-value)
     Read up to N bytes from STREAM, return as a unibyte string. */
  if (nargs < 2) {
    signal_error(env, "cl-read-string requires at least 2 arguments");
    return eintern(env, "nil");
  }

  ptrdiff_t want = (ptrdiff_t)eint_extract(env, args[0]);
  if (want <= 0) return env->make_string(env, "", 0);

  cl_stream_t *s = euser_ptr_stream_get(env, args[1]);
  check_open(env, s, "cl-read-string");

  /* try zero-copy via direct mapping (mmap streams) */
  const void *daddr = NULL;
  size_t dlen = 0;
  if (stream_direct(s, &daddr, &dlen)) {
    size_t to_read = (size_t)want < dlen ? (size_t)want : dlen;
    if (to_read == 0) {
      if (nargs >= 4 && !env->is_not_nil(env, args[2]))
        return (nargs >= 4) ? args[3] : eintern(env, "nil");
      signal_error(env, "End of file on stream");
      return eintern(env, "nil");
    }
    emacs_value result = env->make_unibyte_string(env, (const char *)daddr,
                                                   (ptrdiff_t)to_read);
    stream_advance(s, (ptrdiff_t)to_read);
    return result;
  }

  /* fallback: read into temp buffer */
  unsigned char *buf = malloc((size_t)want);
  if (!buf) { signal_error(env, "Memory allocation failed"); return eintern(env, "nil"); }

  ptrdiff_t r = stream_read(s, buf, want);
  if (r <= 0) {
    free(buf);
    if (nargs >= 4 && !env->is_not_nil(env, args[2]))
      return (nargs >= 4) ? args[3] : eintern(env, "nil");
    signal_error(env, "End of file on stream");
    return eintern(env, "nil");
  }

  emacs_value result = env->make_unibyte_string(env, (const char *)buf, r);
  free(buf);
  return result;
}
