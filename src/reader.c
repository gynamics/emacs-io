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

/* ── cl-read-char ─────────────────────────────────────────────────────── */

emacs_value Fread_char(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                        void *data) {
  (void)data;
  /* (cl-read-char &optional stream eof-error-p eof-value) */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  check_open(env, s, "cl-read-char");

  unsigned char ch;
  ptrdiff_t r = stream_read(s, &ch, 1);
  if (r <= 0) return eof_result(env, s, nargs, args, 1, 2);
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
  if (r <= 0) return eof_result(env, s, nargs, args, 1, 2);
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
    return eof_result(env, s, nargs, args, 1, 2);
  }

  unsigned char ch;
  ptrdiff_t r = stream_read(s, &ch, 1);
  if (r <= 0) {
    if (interactive) return syms.v_nil;
    return eof_result(env, s, nargs, args, 1, 2);
  }
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
    if (r <= 0) return eof_result(env, s, nargs, args, 2, 3);

    if (skip_ws && is_whitespace((int)ch))
      continue;             /* discard whitespace */

    if (skip_to_char && (int)ch != target)
      continue;             /* skip chars before the matching one */

    /* the char to peek at is left in the stream */
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

/* ── cl-read / cl-read-preserving-whitespace ──────────────────────────────
   Reading scans a non-consuming window of the stream (stream_direct) and
   advances by exactly the consumed count (stream_advance).  The stream
   refills its buffer on demand, so nothing is over-read and no pushback is
   needed: reading many lines/objects is linear and cannot lose data. */

/* Cap the window bytes handed to read-from-string in one call, so a small
   form on a large buffered stream does not copy the whole buffer to parse. */
#define READ_WINDOW_CAP 4096

static bool read_ws_byte(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* Parse one form from DATA[0..LEN).  Sets *obj and *pos and returns 1 on
   success; returns 0 when DATA holds only the start of a form; returns -1
   with a pending non-local exit on a genuine reader error. */
static int try_parse(emacs_env *env, const char *data, ptrdiff_t len,
                     emacs_value *obj, ptrdiff_t *pos) {
  emacs_value estr = env->make_string(env, data, len);
  emacs_value raw = efuncall_cached(env, syms.fn_read_from_string, 1, &estr);
  if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
    emacs_value sym = syms.v_nil, data2 = syms.v_nil;
    env->non_local_exit_get(env, &sym, &data2);
    bool is_eof = env->eq(env, sym, syms.v_end_of_file);
    env->non_local_exit_clear(env);
    if (is_eof) return 0;                 /* incomplete */
    env->non_local_exit_signal(env, sym, data2);
    return -1;
  }
  *obj = efuncall_cached(env, syms.fn_car, 1, &raw);
  *pos = eint_extract(env, efuncall_cached(env, syms.fn_cdr, 1, &raw));
  return 1;
}

/* Consume up to END bytes of the logical stream: SB holds the bytes already
   consumed for this form, so any of SB past END is pushed back and bytes
   used from the current window are advanced. */
static void finish_form(cl_stream_t *s, strbuf_t *sb, ptrdiff_t end) {
  if (end < (ptrdiff_t)sb->len)
    stream_unread_bulk(s, (const unsigned char *)sb->data + end,
                       (ptrdiff_t)sb->len - end);
  else if (end > (ptrdiff_t)sb->len)
    stream_advance(s, end - (ptrdiff_t)sb->len);
}

/* Shared by cl-read and cl-read-preserving-whitespace.  With PRESERVE_WS
   false, the single whitespace character that delimits the object is
   discarded, as CL's reader does; with true it is left in the stream. */
static emacs_value read_form(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                             bool preserve_ws) {
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  if (!s || !stream_is_open(s)) {
    signal_error(env, "cl-read: stream is closed");
    return syms.v_nil;
  }

  strbuf_t *sb = strbuf_create(256);   /* bytes already consumed for this form */
  if (!sb) { signal_error(env, "Memory allocation failed"); return syms.v_nil; }

  /* Unread characters precede the buffered window. */
  {
    unsigned char pb[CL_PUSHBACK_SIZE];
    ptrdiff_t npb = stream_read_pushback(s, pb, (ptrdiff_t)sizeof(pb));
    if (npb > 0) strbuf_append(sb, (const char *)pb, npb);
  }

  emacs_value result = syms.v_nil;
  bool have = false;
  ptrdiff_t end = 0;

  /* A form wholly inside the (small) pushback prefix, ending at a delimiter,
     is final.  A token ending exactly at the buffer end is not: it could
     continue in the stream, so wait for more input. */
  if (sb->len > 0) {
    emacs_value obj; ptrdiff_t pos;
    int r = try_parse(env, sb->data, (ptrdiff_t)sb->len, &obj, &pos);
    if (r < 0) { strbuf_free(sb); return syms.v_nil; }
    if (r == 1 && pos < (ptrdiff_t)sb->len) {
      result = obj; end = pos;
      if (!preserve_ws && read_ws_byte(sb->data[end])) end++;
      have = true;
    }
  }

  while (!have) {
    const void *addr; size_t wlen;
    if (!stream_direct(s, &addr, &wlen) || wlen == 0) {
      /* End of input. */
      if (sb->len > 0) {
        emacs_value obj; ptrdiff_t pos;
        int r = try_parse(env, sb->data, (ptrdiff_t)sb->len, &obj, &pos);
        if (r < 0) { strbuf_free(sb); return syms.v_nil; }
        if (r == 1) {
          result = obj; end = pos;
          if (!preserve_ws && end < (ptrdiff_t)sb->len &&
              read_ws_byte(sb->data[end])) end++;
          have = true;
        } else {
          /* An incomplete object at EOF is an error (CL). */
          strbuf_free(sb);
          signal_end_of_file(env, s);
          return syms.v_nil;
        }
      }
      break;
    }

    size_t use = wlen < READ_WINDOW_CAP ? wlen : READ_WINDOW_CAP;

    strbuf_t *comb = strbuf_create(sb->len + use + 1);
    if (!comb) { strbuf_free(sb); signal_error(env, "Memory allocation failed"); return syms.v_nil; }
    strbuf_append(comb, sb->data, (ptrdiff_t)sb->len);
    strbuf_append(comb, (const char *)addr, (ptrdiff_t)use);

    emacs_value obj; ptrdiff_t pos;
    int r = try_parse(env, comb->data, (ptrdiff_t)comb->len, &obj, &pos);

    if (r == 1 && pos < (ptrdiff_t)comb->len) {
      end = pos;
      if (!preserve_ws && read_ws_byte(comb->data[end])) end++;
      result = obj; have = true;
      strbuf_free(comb);
      break;
    }
    strbuf_free(comb);
    if (r < 0) { strbuf_free(sb); return syms.v_nil; }

    /* Incomplete, or complete but ending at the window edge (the token may
       continue in the next block): consume the block and read more. */
    strbuf_append(sb, (const char *)addr, (ptrdiff_t)use);
    stream_advance(s, (ptrdiff_t)use);
  }

  if (!have) {
    strbuf_free(sb);
    return eof_result(env, s, nargs, args, 1, 2);
  }

  finish_form(s, sb, end);
  strbuf_free(sb);
  return result;
}

emacs_value Fread(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                  void *data) {
  (void)data;
  return read_form(env, nargs, args, false);
}

emacs_value Fread_preserving_whitespace(emacs_env *env, ptrdiff_t nargs,
                                         emacs_value *args, void *data) {
  (void)data;
  return read_form(env, nargs, args, true);
}

/* ── cl-read-delimited-list ───────────────────────────────────────────
   (cl-read-delimited-list char &optional stream recursive-p)
   Read objects from STREAM until CHAR is seen, return them as a list. */

/* Try to parse one complete form from the front of SB.  On success stores
   the value in *val and the number of consumed bytes in *pos and returns
   true.  Returns false (clearing any reader exit) when SB only holds an
   incomplete form, so the caller should read more input. */
static bool try_parse_prefix(emacs_env *env, strbuf_t *sb,
                             emacs_value *val, ptrdiff_t *pos) {
  emacs_value estr = env->make_string(env, sb->data, (ptrdiff_t)sb->len);
  emacs_value raw = efuncall_cached(env, syms.fn_read_from_string, 1, &estr);
  if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
    env->non_local_exit_clear(env);
    return false;
  }
  *val = efuncall_cached(env, syms.fn_car, 1, &raw);
  *pos = eint_extract(env, efuncall_cached(env, syms.fn_cdr, 1, &raw));
  return true;
}

/* Discard the first N bytes of SB. */
static void strbuf_drop(strbuf_t *sb, ptrdiff_t n) {
  if (n <= 0) return;
  if ((size_t)n >= sb->len) {
    sb->len = 0;
  } else {
    memmove(sb->data, sb->data + n, sb->len - (size_t)n);
    sb->len -= (size_t)n;
  }
  sb->data[sb->len] = '\0';
}

emacs_value Fread_delimited_list(emacs_env *env, ptrdiff_t nargs,
                                  emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-read-delimited-list requires at least 1 argument");
    return syms.v_nil;
  }

  int delim = (int)eint_extract(env, args[0]);
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 1, syms.var_sin);
  check_open(env, s, "cl-read-delimited-list");

  /* accumulate chars into buffer, call read-from-string at each form */
  strbuf_t *sb = strbuf_create(256);
  if (!sb) { signal_error(env, "Memory allocation failed"); return syms.v_nil; }

  emacs_value lst = syms.v_nil;

  for (;;) {
    unsigned char ch;
    ptrdiff_t r = stream_read(s, &ch, 1);
    if (r <= 0) {
      /* CL: it is an error to reach end-of-file during the operation */
      strbuf_free(sb);
      signal_end_of_file(env, s);
      return syms.v_nil;
    }

    if ((int)ch == delim)
      break;  /* delimiter consumed per CL spec */

    /* skip whitespace between forms */
    if (sb->len == 0 && (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'))
      continue;

    strbuf_append(sb, (const char *)&ch, 1);

    emacs_value val;
    ptrdiff_t pos;
    if (!try_parse_prefix(env, sb, &val, &pos))
      continue;  /* incomplete form — need more data */

    if (pos > 0) {
      emacs_value pair[2] = { val, lst };
      lst = efuncall_cached(env, syms.fn_cons, 2, pair);
      strbuf_drop(sb, pos);
    }
  }

  strbuf_free(sb);
  return efuncall(env, "reverse", 1, &lst);
}
