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
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  check_open(env, s, "cl-read-char");

  unsigned char ch;
  ptrdiff_t r = stream_read(s, &ch, 1);
  if (r <= 0) {
    if (nargs >= 2 && !env->is_not_nil(env, args[1])) /* eof-error-p nil */
      return (nargs >= 3) ? args[2] : syms.v_nil;
    signal_error(env, "End of file on stream");
    return syms.v_nil;
  }
  return eint(env, ch);
}

/* ── cl-read-line ─────────────────────────────────────────────────────── */

#define READ_LINE_CHUNK 4096

emacs_value Fread_line(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                       void *data) {
  (void)data;
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  check_open(env, s, "cl-read-line");

  strbuf_t *sb = strbuf_create(4096);
  if (!sb) { signal_error(env, "Memory allocation failed"); return syms.v_nil; }

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
      if (!strbuf_append(sb, (const char *)chunk, nl_off)) {
        strbuf_free(sb);
        signal_error(env, "Memory allocation failed");
        return syms.v_nil;
      }
      /* push leftover in forward order (FIFO queue: first in → first out) */
      for (ptrdiff_t i = nl_off + skip; i < r; i++)
        stream_unread(s, chunk[i]);
      saw_newline = true;
      break;
    }

    /* no newline found — keep accumulating */
    if (!strbuf_append(sb, (const char *)chunk, r)) {
      strbuf_free(sb);
      signal_error(env, "Memory allocation failed");
      return syms.v_nil;
    }
  }

  if (sb->len == 0 && !saw_newline) {
    strbuf_free(sb);
    if (nargs >= 2 && !env->is_not_nil(env, args[1]))
      return (nargs >= 3) ? args[2] : syms.v_nil;
    signal_error(env, "End of file on stream");
    return syms.v_nil;
  }

  emacs_value line = strbuf_to_estring(env, sb);
  strbuf_free(sb);

  emacs_value cons_args[2];
  cons_args[0] = line;
  cons_args[1] = saw_newline ? syms.v_nil : syms.v_t;
  return efuncall_cached(env, syms.fn_list, 2, cons_args);
}

/* ── cl-read-byte ─────────────────────────────────────────────────────── */

emacs_value Fread_byte(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                       void *data) {
  (void)data;
  /* (cl-read-byte stream &optional eof-error-p eof-value) */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  check_open(env, s, "cl-read-byte");

  unsigned char b;
  ptrdiff_t r = stream_read(s, &b, 1);
  if (r <= 0) {
    if (nargs >= 2 && !env->is_not_nil(env, args[1]))
      return (nargs >= 3) ? args[2] : syms.v_nil;
    signal_error(env, "End of file on stream");
    return syms.v_nil;
  }
  return eint(env, b);
}

/* ── cl-read-char-no-hang ──────────────────────────────────────────────── */

emacs_value Fread_char_no_hang(emacs_env *env, ptrdiff_t nargs,
                                emacs_value *args, void *data) {
  (void)data;
  /* (cl-read-char-no-hang &optional stream eof-error-p eof-value) */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  check_open(env, s, "cl-read-char-no-hang");

  if (!stream_listen(s)) return syms.v_nil;

  unsigned char ch;
  ptrdiff_t r = stream_read(s, &ch, 1);
  if (r <= 0) return syms.v_nil;
  return eint(env, ch);
}

/* ── cl-peek-char ──────────────────────────────────────────────────────── */

static bool is_whitespace(int ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

emacs_value Fpeek_char(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                       void *data) {
  (void)data;
  /* (cl-peek-char &optional peek-type stream eof-error-p eof-value) */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs,
                                          (nargs >= 1) ? 1 : 0,
                                          syms.var_sin);
  check_open(env, s, "cl-peek-char");

  /*
   * peek-type nil  → peek at next char without consuming
   * peek-type t    → skip whitespace, then peek
   * peek-type char → skip until that char seen, then peek at next
   */
  emacs_value peek_type = (nargs >= 1) ? args[0] : syms.v_nil;
  bool skip_ws = env->eq(env, peek_type, syms.v_t);
  bool skip_to_char = false;
  int target = 0;

  if (!skip_ws && env->is_not_nil(env, peek_type)) {
    skip_to_char = true;
    target = (int)eint_extract(env, peek_type);
  }

  for (;;) {
    if (!stream_listen(s)) return syms.v_nil;

    unsigned char ch;
    ptrdiff_t r = stream_read(s, &ch, 1);
    if (r <= 0) return syms.v_nil;

    if (skip_ws && is_whitespace((int)ch))
      continue;             /* discard whitespace */

    if (skip_to_char && (int)ch == target)
      continue;             /* discard matching char */

    /* found the char to peek — push it back and return */
    stream_unread(s, ch);
    return eint(env, ch);
  }
}

/* ── cl-unread-char ────────────────────────────────────────────────────── */

emacs_value Funread_char(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                         void *data) {
  (void)data;
  (void)nargs;
  (void)args;
  if (nargs < 1) { signal_error(env, "cl-unread-char requires at least 1 argument"); return syms.v_nil; }
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs,
                                          (nargs >= 2) ? 1 : 0,
                                          syms.var_sin);
  intmax_t ch = eint_extract(env, args[0]);
  stream_unread(s, (int)ch);
  return syms.v_nil;
}

/* ── cl-listen ────────────────────────────────────────────────────────── */

emacs_value Flisten(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                    void *data) {
  (void)data;
  /* (cl-listen &optional stream) */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  if (!s || !stream_is_open(s)) return syms.v_nil;

  return stream_listen(s) ? syms.v_t : syms.v_nil;
}

/* ── cl-read-sequence ──────────────────────────────────────────────────── */

emacs_value Fread_sequence(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                           void *data) {
  (void)data;
  /* (cl-read-sequence sequence stream &key start end) */
  if (nargs < 2) {
    signal_error(env, "cl-read-sequence requires at least 2 arguments");
    return syms.v_nil;
  }

  emacs_value seq = args[0];
  cl_stream_t *s = euser_ptr_stream_get(env, args[1]);
  if (!s || !stream_is_open(s))
    signal_error(env, "Stream is closed");

  bool is_vec = env->eq(env, env->type_of(env, seq), eintern(env, "vector"));
  bool is_list = !is_vec && env->eq(env, env->type_of(env, seq),
                                     eintern(env, "cons"));
  if (!is_vec && !is_list) {
    signal_error(env, "cl-read-sequence: SEQUENCE must be a vector or list");
    return syms.v_nil;
  }

  ptrdiff_t len = is_vec ? env->vec_size(env, seq)
                         : 1 << 30;
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
  if (count <= 0) return eint(env, start);

  if (is_vec) {
    unsigned char *buf = malloc((size_t)count);
    if (!buf) { signal_error(env, "Memory allocation failed"); return syms.v_nil; }

    ptrdiff_t r = stream_read(s, buf, count);
    for (ptrdiff_t i = 0; i < r && i < count; i++)
      env->vec_set(env, seq, start + i, eint(env, buf[i]));

    free(buf);
    return eint(env, start + (r > 0 ? r : 0));
  }

  /* list: advance to start, read one byte per element */
  emacs_value cell = seq;
  for (ptrdiff_t i = 0; i < start && env->is_not_nil(env, cell); i++)
    cell = efuncall(env, "cdr", 1, &cell);

  ptrdiff_t i = 0;
  while (i < count && env->is_not_nil(env, cell)) {
    unsigned char b;
    ptrdiff_t r = stream_read(s, &b, 1);
    if (r <= 0) break;
    efuncall(env, "setcar", 2, (emacs_value[]){ cell, eint(env, b) });
    cell = efuncall(env, "cdr", 1, &cell);
    i++;
  }
  return eint(env, start + i);
}

/* ── cl-read-string ────────────────────────────────────────────────────── */

emacs_value Fread_string(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                          void *data) {
  (void)data;
  /* (cl-read-string n stream &optional eof-error-p eof-value)
     Read up to N bytes from STREAM, return as a unibyte string. */
  if (nargs < 2) {
    signal_error(env, "cl-read-string requires at least 2 arguments");
    return syms.v_nil;
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
        return (nargs >= 4) ? args[3] : syms.v_nil;
      signal_error(env, "End of file on stream");
      return syms.v_nil;
    }
    emacs_value result = env->make_unibyte_string(env, (const char *)daddr,
                                                   (ptrdiff_t)to_read);
    stream_advance(s, (ptrdiff_t)to_read);
    return result;
  }

  /* fallback: read into temp buffer */
  unsigned char *buf = malloc((size_t)want);
  if (!buf) { signal_error(env, "Memory allocation failed"); return syms.v_nil; }

  ptrdiff_t r = stream_read(s, buf, want);
  if (r <= 0) {
    free(buf);
    if (nargs >= 4 && !env->is_not_nil(env, args[2]))
      return (nargs >= 4) ? args[3] : syms.v_nil;
    signal_error(env, "End of file on stream");
    return syms.v_nil;
  }

  emacs_value result = env->make_unibyte_string(env, (const char *)buf, r);
  free(buf);
  return result;
}

/* ── cl-read-chunk ─────────────────────────────────────────────────────── */

emacs_value Fread_chunk(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                         void *data) {
  (void)data;
  /* (cl-read-chunk stream &optional max-bytes)
     Read up to MAX-BYTES (default 64KB), return (CHUNK . EOF-P). */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  check_open(env, s, "cl-read-chunk");

  ptrdiff_t max = 65536;
  if (nargs >= 2) max = eint_extract(env, args[1]);
  if (max <= 0) max = 65536;

  /* try zero-copy via direct mapping */
  const void *daddr = NULL;
  size_t dlen = 0;
  if (stream_direct(s, &daddr, &dlen)) {
    size_t n = (size_t)max < dlen ? (size_t)max : dlen;
    emacs_value chunk = env->make_unibyte_string(env, (const char *)daddr,
                                                  (ptrdiff_t)n);
    stream_advance(s, (ptrdiff_t)n);
    emacs_value eof = (dlen - n == 0) ? syms.v_t : syms.v_nil;
    emacs_value pair_args[2];
    pair_args[0] = chunk;
    pair_args[1] = eof;
    return efuncall_cached(env, syms.fn_cons, 2, pair_args);
  }

  unsigned char *buf = malloc((size_t)max);
  if (!buf) { signal_error(env, "Memory allocation failed"); return syms.v_nil; }

  ptrdiff_t r = stream_read(s, buf, max);
  emacs_value chunk = env->make_unibyte_string(env, (const char *)buf, r > 0 ? r : 0);
  free(buf);

  emacs_value eof = (r <= 0) ? syms.v_t : syms.v_nil;
  emacs_value pair_args[2];
  pair_args[0] = chunk;
  pair_args[1] = eof;
  return efuncall_cached(env, syms.fn_cons, 2, pair_args);
}
