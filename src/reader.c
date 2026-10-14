#include "emacs-module.h"
#include "stream.h"
#include "util.h"
#include "subrs.h"
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

/* ── cl-unread-char bookkeeping ─────────────────────────────────────────
   CL requires the unread character to be the most recently read one, and
   forbids unreading twice without an intervening read.  These helpers keep
   the per-stream state that Funread_char validates. */

static void note_char_read(cl_stream_t *s, int ch) {
  if (!s) return;
  s->unread_last = ch;
  s->unread_last_valid = true;
  s->unread_pending = false;
}

/* Any other read (EOF, bulk, multi-character) clears the "last char" so a
   following unread cannot claim a stale character. */
static void note_read_reset(cl_stream_t *s) {
  if (!s) return;
  s->unread_last_valid = false;
  s->unread_pending = false;
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
  if (r <= 0) { note_read_reset(s); return eof_result(env, s, nargs, args, 1, 2); }
  note_char_read(s, (int)ch);
  return eint(env, ch);
}

/* ── cl-read-line ─────────────────────────────────────────────────────── */

/* Scan P[0..N) for a line terminator.  On success set *NL to its index and
   *SKIP to the bytes to consume (1 for \n or a lone \r, 2 for \r\n). */
static bool scan_newline(const unsigned char *p, size_t n,
                         ptrdiff_t *nl, ptrdiff_t *skip) {
  for (size_t i = 0; i < n; i++) {
    if (p[i] == '\n') { *nl = (ptrdiff_t)i; *skip = 1; return true; }
    if (p[i] == '\r') {
      *nl = (ptrdiff_t)i;
      *skip = (i + 1 < n && p[i + 1] == '\n') ? 2 : 1;
      return true;
    }
  }
  return false;
}

emacs_value Fread_line(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                       void *data) {
  (void)data;
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  check_open(env, s, "cl-read-line");
  note_read_reset(s);

  strbuf_t *sb = strbuf_create(256);
  if (!sb) { signal_error(env, "Memory allocation failed"); return syms.v_nil; }

  bool saw_newline = false;

  /* Unread characters (cl-unread-char) precede the buffered window. */
  {
    unsigned char pb[CL_PUSHBACK_SIZE];
    ptrdiff_t npb = stream_read_pushback(s, pb, (ptrdiff_t)sizeof(pb));
    if (npb > 0) {
      ptrdiff_t nl, skip;
      if (scan_newline(pb, (size_t)npb, &nl, &skip)) {
        strbuf_append(sb, (const char *)pb, nl);
        if (nl + skip < npb)
          stream_unread_bulk(s, pb + nl + skip, npb - (nl + skip));
        saw_newline = true;
      } else if (!strbuf_append(sb, (const char *)pb, npb)) {
        strbuf_free(sb);
        signal_error(env, "Memory allocation failed");
        return syms.v_nil;
      }
    }
  }

  /* Scan successive non-consuming windows; advance only past the newline. */
  while (!saw_newline) {
    const void *addr; size_t wlen;
    if (!stream_direct(s, &addr, &wlen) || wlen == 0) break;   /* EOF */

    ptrdiff_t nl, skip;
    if (scan_newline((const unsigned char *)addr, wlen, &nl, &skip)) {
      if (!strbuf_append(sb, (const char *)addr, nl)) {
        strbuf_free(sb);
        signal_error(env, "Memory allocation failed");
        return syms.v_nil;
      }
      stream_advance(s, nl + skip);
      saw_newline = true;
    } else {
      if (!strbuf_append(sb, (const char *)addr, (ptrdiff_t)wlen)) {
        strbuf_free(sb);
        signal_error(env, "Memory allocation failed");
        return syms.v_nil;
      }
      stream_advance(s, (ptrdiff_t)wlen);
    }
  }

  /* CL returns two values (line, missing-newline-p).  Elisp has no multiple
     values, so a dotted pair (line . missing-newline-p) is returned. */
  if (sb->len == 0 && !saw_newline) {
    /* immediate EOF: (eof-value . t), or signal when eof-error-p is true */
    strbuf_free(sb);
    if (nargs >= 2 && !env->is_not_nil(env, args[1])) {
      emacs_value eof_pair[2];
      eof_pair[0] = (nargs >= 3) ? args[2] : syms.v_nil;
      eof_pair[1] = syms.v_t;
      return efuncall_cached(env, syms.fn_cons, 2, eof_pair);
    }
    signal_end_of_file(env, s);
    return syms.v_nil;
  }

  emacs_value line = strbuf_to_estring(env, sb);
  strbuf_free(sb);

  emacs_value cons_args[2];
  cons_args[0] = line;
  cons_args[1] = saw_newline ? syms.v_nil : syms.v_t;
  return efuncall_cached(env, syms.fn_cons, 2, cons_args);
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
  if (r <= 0) { note_read_reset(s); return eof_result(env, s, nargs, args, 1, 2); }
  note_char_read(s, (int)b);
  return eint(env, b);
}

/* ── cl-read-char-no-hang ──────────────────────────────────────────────── */

emacs_value Fread_char_no_hang(emacs_env *env, ptrdiff_t nargs,
                                emacs_value *args, void *data) {
  (void)data;
  /* (cl-read-char-no-hang &optional stream eof-error-p eof-value) */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  check_open(env, s, "cl-read-char-no-hang");

  /* On an interactive stream, "no character available" means it would block
     -> nil.  On a non-interactive stream it means end of file, which the
     usual eof-error-p / eof-value handling applies to (CL). */
  bool interactive = (s->flags & CL_STREAM_FLAG_INTERACTIVE) != 0;

  if (!stream_listen(s)) {
    if (interactive) return syms.v_nil;
    note_read_reset(s);
    return eof_result(env, s, nargs, args, 1, 2);
  }

  unsigned char ch;
  ptrdiff_t r = stream_read(s, &ch, 1);
  if (r <= 0) {
    if (interactive) return syms.v_nil;
    note_read_reset(s);
    return eof_result(env, s, nargs, args, 1, 2);
  }
  note_char_read(s, (int)ch);
  return eint(env, ch);
}

/* ── cl-peek-char ──────────────────────────────────────────────────────── */

static bool is_whitespace(int ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

emacs_value Fpeek_char(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                       void *data) {
  (void)data;
  /* (cl-peek-char &optional peek-type stream eof-error-p eof-value recursive-p) */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs,
                                          (nargs >= 1) ? 1 : 0,
                                          syms.var_sin);
  check_open(env, s, "cl-peek-char");

  /*
   * peek-type nil  → peek at the next char without consuming it
   * peek-type t    → skip whitespace, then peek
   * peek-type char → skip chars that are not char, then peek the matching
   *                  char (it is left in the stream), per CLHS
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
    unsigned char ch;
    ptrdiff_t r = stream_read(s, &ch, 1);
    if (r <= 0) { note_read_reset(s); return eof_result(env, s, nargs, args, 2, 3); }

    if (skip_ws && is_whitespace((int)ch))
      continue;             /* discard whitespace */

    if (skip_to_char && (int)ch != target)
      continue;             /* skip chars before the matching one */

    /* the char to peek at is left in the stream */
    stream_unread(s, ch);
    s->unread_last = (int)ch;
    s->unread_last_valid = true;
    s->unread_pending = true;
    return eint(env, ch);
  }
}

/* ── cl-unread-char ────────────────────────────────────────────────────── */

emacs_value Funread_char(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                         void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-unread-char requires at least 1 argument");
    return syms.v_nil;
  }
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs,
                                          (nargs >= 2) ? 1 : 0,
                                          syms.var_sin);
  if (!s || !stream_is_open(s)) {
    signal_error(env, "cl-unread-char: stream is closed");
    return syms.v_nil;
  }

  /* CL: CHARACTER must be the most recently read character, and it is an
     error to unread twice without an intervening read. */
  int ch = (int)eint_extract(env, args[0]);
  if (s->unread_pending) {
    signal_error(env,
      "cl-unread-char: cannot unread twice without an intervening read");
    return syms.v_nil;
  }
  if (!s->unread_last_valid || ch != s->unread_last) {
    signal_error(env,
      "cl-unread-char: CHARACTER must be the most recent character read");
    return syms.v_nil;
  }

  stream_unread(s, ch);
  s->unread_pending = true;
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
  note_read_reset(s);

  ptrdiff_t len;
  cl_seq_kind_t kind = seq_classify(env, seq, &len);
  if (kind == CL_SEQ_INVALID) {
    signal_error(env, "cl-read-sequence: SEQUENCE must be a vector, list, or string");
    return syms.v_nil;
  }
  bool is_vec = kind == CL_SEQ_VECTOR;
  bool is_str = kind == CL_SEQ_STRING;

  ptrdiff_t start = 0;
  ptrdiff_t end = len;

  for (ptrdiff_t i = 2; i < nargs; i += 2) {
    if (i + 1 >= nargs) break;
    if (kw_eq_cached(env, args[i], syms.k_start))
      start = eint_extract(env, args[i + 1]);
    else if (kw_eq_cached(env, args[i], syms.k_end))
      end = eint_extract(env, args[i + 1]);
  }

  /* CL: start and end must be valid bounding indices. */
  if (start < 0 || end < 0 || end < start ||
      ((is_vec || is_str) && (start > len || end > len))) {
    signal_error_type(env, "args-out-of-range",
                      "cl-read-sequence: invalid bounding index");
    return syms.v_nil;
  }
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

  if (is_str) {
    unsigned char *buf = malloc((size_t)count);
    if (!buf) { signal_error(env, "Memory allocation failed"); return syms.v_nil; }

    ptrdiff_t r = stream_read(s, buf, count);
    emacs_value aset_sym = eintern(env, "aset");
    for (ptrdiff_t i = 0; i < r && i < count; i++) {
      emacs_value av[3] = { seq, eint(env, start + i), eint(env, buf[i]) };
      env->funcall(env, aset_sym, 3, av);
    }
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
  note_read_reset(s);

  /* try zero-copy via direct mapping (mmap streams) */
  const void *daddr = NULL;
  size_t dlen = 0;
  if (stream_direct(s, &daddr, &dlen)) {
    size_t to_read = (size_t)want < dlen ? (size_t)want : dlen;
    if (to_read == 0) {
      if (nargs >= 4 && !env->is_not_nil(env, args[2])) return args[3];
      signal_end_of_file(env, s);
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
    if (nargs >= 4 && !env->is_not_nil(env, args[2])) return args[3];
    signal_end_of_file(env, s);
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
  note_read_reset(s);

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

