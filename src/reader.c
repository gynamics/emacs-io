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

/* Resolve the end-of-file behaviour for the readers whose signature is
   (fn &optional STREAM EOF-ERROR-P EOF-VALUE): when EOF-ERROR-P is nil,
   return EOF-VALUE (or nil); otherwise signal `end-of-file'. */
static emacs_value read_eof(emacs_env *env, cl_stream_t *s, ptrdiff_t nargs,
                            emacs_value *args) {
  if (nargs >= 2 && !env->is_not_nil(env, args[1]))
    return (nargs >= 3) ? args[2] : syms.v_nil;
  signal_end_of_file(env, s);
  return syms.v_nil;
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
  if (r <= 0) return read_eof(env, s, nargs, args);
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
    return read_eof(env, s, nargs, args);
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
  if (r <= 0) return read_eof(env, s, nargs, args);
  return eint(env, b);
}

/* ── cl-read-char-no-hang ──────────────────────────────────────────────── */

emacs_value Fread_char_no_hang(emacs_env *env, ptrdiff_t nargs,
                                emacs_value *args, void *data) {
  (void)data;
  /* (cl-read-char-no-hang &optional stream eof-error-p eof-value) */
  cl_stream_t *s = get_stream_or_default_cached(env, args, nargs, 0, syms.var_sin);
  check_open(env, s, "cl-read-char-no-hang");

  if (!stream_listen(s)) {
    if (nargs >= 2 && !env->is_not_nil(env, args[1]))
      return (nargs >= 3) ? args[2] : syms.v_nil;
    return syms.v_nil;
  }

  unsigned char ch;
  ptrdiff_t r = stream_read(s, &ch, 1);
  if (r <= 0) {
    if (nargs >= 2 && !env->is_not_nil(env, args[1]))
      return (nargs >= 3) ? args[2] : syms.v_nil;
    if (nargs >= 2 && env->is_not_nil(env, args[1]))
      { signal_end_of_file(env, s); return syms.v_nil; }
    return syms.v_nil;
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

  if (start < 0) start = 0;
  if ((is_vec || is_str) && end > len) end = len;
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

/* ── cl-read-preserving-whitespace ────────────────────────────────────
   Like cl-read but preserves trailing whitespace.
   Our cl-read already preserves whitespace (pushes back unconsumed bytes),
   so this is effectively an alias. */

emacs_value Fread_preserving_whitespace(emacs_env *env, ptrdiff_t nargs,
                                         emacs_value *args, void *data) {
  extern emacs_value Fread(emacs_env *, ptrdiff_t, emacs_value *, void *);
  return Fread(env, nargs, args, data);
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
  bool had_data = false;

  for (;;) {
    if (!stream_listen(s)) break;
    unsigned char ch;
    ptrdiff_t r = stream_read(s, &ch, 1);
    if (r <= 0) break;

    if ((int)ch == delim)
      break;  /* delimiter consumed per CL spec */

    had_data = true;
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

  if (!had_data && env->is_not_nil(env, lst)) {
    /* nothing parsed but delimiter found at start — return nil */
    return syms.v_nil;
  }

  return efuncall(env, "reverse", 1, &lst);
}

/* ── yes/no queries on *query-io* ────────────────────────────────────
   Shared by cl-y-or-n-p and cl-yes-or-no-p, which differ only in the
   prompt suffix and in how much of the answer must match. */

/* Match a leading "y" (full == false) or "yes" (full == true). */
static bool answer_is_yes(const char *s, ptrdiff_t len, bool full) {
  if (len < 1 || (s[0] != 'y' && s[0] != 'Y')) return false;
  if (!full) return true;
  return len >= 3
      && (s[1] == 'e' || s[1] == 'E')
      && (s[2] == 's' || s[2] == 'S');
}

/* Write the formatted prompt (if any) followed by SUFFIX to S, read one
   line from S, and return whether the answer matched.  Signals and
   returns false if *query-io* is not an open stream. */
static bool ask_query(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                      cl_stream_t *s, const char *suffix, bool full) {
  if (nargs >= 1 && env->is_not_nil(env, args[0])) {
    emacs_value fmt_args[16];
    fmt_args[0] = args[0];
    ptrdiff_t nfmt = nargs < 16 ? nargs : 16;
    for (ptrdiff_t i = 1; i < nfmt; i++)
      fmt_args[i] = args[i];

    emacs_value prompt = env->funcall(env, eintern(env, "format"), nfmt, fmt_args);
    stream_write_string(env, s, prompt);
    stream_write_string(env, s, estring(env, suffix));
  }

  strbuf_t *sb = strbuf_create(128);
  if (!sb) return false;
  for (;;) {
    unsigned char ch;
    ptrdiff_t r = stream_read(s, &ch, 1);
    if (r <= 0) break;
    if (ch == '\n' || ch == '\r') break;
    strbuf_append(sb, (const char *)&ch, 1);
  }

  emacs_value resp = strbuf_to_estring(env, sb);
  strbuf_free(sb);

  ptrdiff_t rlen = 0;
  char *rstr = estring_alloc(env, resp, &rlen);
  if (!rstr) return false;
  bool result = answer_is_yes(rstr, rlen, full);
  free(rstr);
  return result;
}

/* Resolve *query-io*, then run ask_query against it. */
static emacs_value query_io(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                            const char *fnname, const char *suffix, bool full) {
  emacs_value qio = efuncall_cached(env, syms.fn_symbol_value, 1,
                     (emacs_value[]){ eintern(env, "*query-io*") });
  cl_stream_t *s = euser_ptr_stream_get(env, qio);
  if (!s || !stream_is_open(s)) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s: *query-io* is not an open stream", fnname);
    signal_error(env, buf);
    return syms.v_nil;
  }
  return ask_query(env, nargs, args, s, suffix, full) ? syms.v_t : syms.v_nil;
}

/* ── cl-y-or-n-p ─────────────────────────────────────────────────────
   (cl-y-or-n-p &optional control &rest arguments) */

emacs_value Fy_or_n_p(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                       void *data) {
  (void)data;
  return query_io(env, nargs, args, "y-or-n-p", " (y/n) ", false);
}

/* ── cl-yes-or-no-p ──────────────────────────────────────────────────
   (cl-yes-or-no-p &optional control &rest arguments) */

emacs_value Fyes_or_no_p(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                          void *data) {
  (void)data;
  return query_io(env, nargs, args, "yes-or-no-p", " (yes/no) ", true);
}
