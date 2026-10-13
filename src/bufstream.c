#include "stream.h"
#include "util.h"
#include "subrs.h"
#include <stdlib.h>
#include <string.h>

/* ── buffer output helpers ──────────────────────────────────────────────
   Shared by buffer-output and bidirectional buffer streams. */

/* Insert STR at point of BUFREF without disturbing the current buffer. */
static void buf_insert_str(emacs_env *env, emacs_value bufref, emacs_value str) {
  emacs_value old = efuncall(env, "current-buffer", 0, NULL);
  efuncall(env, "set-buffer", 1, &bufref);
  efuncall(env, "insert", 1, &str);
  efuncall(env, "set-buffer", 1, &old);
}

static void buf_insert(emacs_env *env, emacs_value bufref,
                        const char *data, ptrdiff_t len) {
  if (len <= 0) return;
  buf_insert_str(env, bufref, env->make_string(env, data, len));
}

static void buf_flush_strbuf(emacs_env *env, emacs_value bufref, strbuf_t *sb) {
  if (!sb || sb->len == 0) return;
  buf_insert_str(env, bufref, env->make_string(env, sb->data, (ptrdiff_t)sb->len));
  sb->len = 0; sb->data[0] = '\0';
}

/* Flush any buffered byte writes, then insert STR verbatim.  Returns the
   number of characters inserted (the string length), or -1 on failure. */
static ptrdiff_t buffer_write_string(emacs_env *env, emacs_value bufref,
                                     strbuf_t *sb, emacs_value str) {
  if (sb->len > 0) buf_flush_strbuf(env, bufref, sb);
  ptrdiff_t slen = 0;
  char *cstr = estring_alloc(env, str, &slen);
  if (!cstr) return -1;
  buf_insert(env, bufref, cstr, slen);
  free(cstr);
  return slen;
}

/* Copy up to N bytes from a fixed snapshot starting at *POS, advancing it. */
static ptrdiff_t snapshot_read(const char *data, size_t len, size_t *pos,
                               void *buf, ptrdiff_t n) {
  if (!data || *pos >= len) return 0;
  size_t remaining = len - *pos;
  size_t to_copy = (size_t)n < remaining ? (size_t)n : remaining;
  memcpy(buf, data + *pos, to_copy);
  *pos += to_copy;
  return (ptrdiff_t)to_copy;
}

/* Resolve :START / :END keyword arguments for a buffer stream whose
   buffer object is args[0].  Defaults are 0 and the buffer size. */
static void buffer_region(emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                          ptrdiff_t *start, ptrdiff_t *end) {
  *start = 0;
  *end = eint_extract(env,
    efuncall_cached(env, syms.fn_buffer_size, 1, &args[0]));
  for (ptrdiff_t i = 1; i < nargs - 1; i += 2) {
    if (kw_eq_cached(env, args[i], syms.k_start))
      *start = eint_extract(env, args[i + 1]);
    else if (kw_eq_cached(env, args[i], syms.k_end))
      *end = eint_extract(env, args[i + 1]);
  }
  if (*start < 0) *start = 0;
  if (*end < *start) *end = *start;
}

static char *buf_snapshot(emacs_env *env, emacs_value buffer,
                           ptrdiff_t start, ptrdiff_t end, size_t *out_len) {
  emacs_value old = efuncall(env, "current-buffer", 0, NULL);
  efuncall(env, "set-buffer", 1, &buffer);
  emacs_value args[2];
  args[0] = eint(env, start + 1);   /* Emacs positions are 1-indexed */
  args[1] = eint(env, end + 1);
  emacs_value text = efuncall_cached(env, syms.fn_buffer_substring, 2, args);
  efuncall(env, "set-buffer", 1, &old);

  ptrdiff_t slen = 0;
  if (!env->copy_string_contents(env, text, NULL, &slen)) { *out_len = 0; return NULL; }
  if (slen <= 1) { *out_len = 0; return NULL; }

  char *data = malloc((size_t)slen);
  if (!data) { *out_len = 0; return NULL; }
  ptrdiff_t copied = slen;
  if (!env->copy_string_contents(env, text, data, &copied)) {
    free(data); *out_len = 0; return NULL;
  }
  *out_len = (size_t)(copied - 1);
  return data;
}

/* ── buffer input stream ─────────────────────────────────────────────── */

static ptrdiff_t bis_read(cl_stream_t *s, void *buf, ptrdiff_t n);
static int       bis_close(cl_stream_t *s);
static bool      bis_listen(cl_stream_t *s);
static void      bis_destroy(cl_stream_t *s);
static ptrdiff_t bis_file_position(cl_stream_t *s);
static bool      bis_set_file_position(cl_stream_t *s, ptrdiff_t pos);
static ptrdiff_t bis_file_length(cl_stream_t *s);

static const cl_stream_ops_t buffer_input_ops = {
  .read    = bis_read,  .write   = NULL,
  .close   = bis_close, .flush   = NULL,
  .finish  = NULL,      .listen  = bis_listen,
  .destroy = bis_destroy,
  .file_position = bis_file_position,
  .set_file_position = bis_set_file_position,
  .file_length    = bis_file_length,
};

typedef struct {
  char   *data;
  size_t  len;
  size_t  pos;
} buffer_input_full_t;

cl_stream_t *stream_create_buffer_input(emacs_env *env, emacs_value buffer,
                                        ptrdiff_t start, ptrdiff_t end) {
  size_t slen = 0;
  char *data = buf_snapshot(env, buffer, start, end, &slen);
  if (!data) return NULL;

  buffer_input_full_t *bi = calloc(1, sizeof(*bi));
  if (!bi) { free(data); return NULL; }
  bi->data = data; bi->len = slen; bi->pos = 0;

  cl_stream_t *s = stream_alloc(&buffer_input_ops, bi,
                                CL_STREAM_KIND_BUFFER_INPUT, CL_STREAM_FLAG_INPUT);
  if (!s) { free(bi->data); free(bi); return NULL; }
  return s;
}

static ptrdiff_t bis_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (!bi) return 0;
  return snapshot_read(bi->data, bi->len, &bi->pos, buf, n);
}

static int bis_close(cl_stream_t *s) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (bi) { free(bi->data); bi->data = NULL; bi->len = 0; bi->pos = 0; }
  atomic_store(&s->open, false);
  return 0;
}

static void bis_destroy(cl_stream_t *s) { free(s->impl); }
static bool bis_listen(cl_stream_t *s) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  return bi != NULL && bi->pos < bi->len;
}
static ptrdiff_t bis_file_position(cl_stream_t *s) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (!bi) return -1;
  return (ptrdiff_t)bi->pos;
}
static bool bis_set_file_position(cl_stream_t *s, ptrdiff_t pos) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (!bi || pos < 0 || (size_t)pos > bi->len) return false;
  bi->pos = (size_t)pos;
  return true;
}
static ptrdiff_t bis_file_length(cl_stream_t *s) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (!bi) return -1;
  return (ptrdiff_t)bi->len;
}

emacs_value Fmake_buffer_input_stream(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-make-buffer-input-stream requires 1 argument");
    return syms.v_nil;
  }
  ptrdiff_t start, end;
  buffer_region(env, nargs, args, &start, &end);
  return stream_or_error(env, stream_create_buffer_input(env, args[0], start, end),
                         "Cannot create buffer input stream");
}

/* ── buffer output stream ────────────────────────────────────────────── */

static ptrdiff_t bos_write(cl_stream_t *s, const void *buf, ptrdiff_t n);
static ptrdiff_t bos_write_string(emacs_env *env, cl_stream_t *s, emacs_value str);
static int       bos_close(cl_stream_t *s);
static void      bos_destroy(cl_stream_t *s);
static void      bos_clear_output(cl_stream_t *s);
static ptrdiff_t bos_file_position(cl_stream_t *s);
static ptrdiff_t bos_file_length(cl_stream_t *s);

const cl_stream_ops_t buffer_output_ops = {
  .read         = NULL,          .write        = bos_write,
  .write_string = bos_write_string, .close    = bos_close,
  .flush        = NULL,          .finish       = NULL,
  .listen       = NULL,          .destroy      = bos_destroy,
  .clear_output = bos_clear_output,
  .file_position = bos_file_position, .file_length = bos_file_length,
};

typedef struct {
  emacs_value buffer_ref;
  strbuf_t   *sb;
} buffer_output_full_t;

static ptrdiff_t bos_write(cl_stream_t *s, const void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  buffer_output_full_t *bo = (buffer_output_full_t *)s->impl;
  if (!bo) return -1;
  if (!strbuf_append(bo->sb, (const char *)buf, n)) return -1;
  return n;
}

static ptrdiff_t bos_write_string(emacs_env *env, cl_stream_t *s, emacs_value str) {
  buffer_output_full_t *bo = (buffer_output_full_t *)s->impl;
  if (!bo) return -1;
  /* flush accumulated byte writes, then insert the string directly */
  return buffer_write_string(env, bo->buffer_ref, bo->sb, str);
}

void bos_flush_pending(emacs_env *env, cl_stream_t *s) {
  buffer_output_full_t *bo = (buffer_output_full_t *)s->impl;
  if (bo) buf_flush_strbuf(env, bo->buffer_ref, bo->sb);
}

static int bos_close(cl_stream_t *s) {
  buffer_output_full_t *bo = (buffer_output_full_t *)s->impl;
  if (bo) { strbuf_free(bo->sb); bo->sb = NULL; }
  atomic_store(&s->open, false);
  return 0;
}

static void bos_destroy(cl_stream_t *s) { free(s->impl); }

static void bos_clear_output(cl_stream_t *s) {
  buffer_output_full_t *bo = (buffer_output_full_t *)s->impl;
  if (bo && bo->sb) { bo->sb->len = 0; bo->sb->data[0] = '\0'; }
}

static ptrdiff_t bos_file_position(cl_stream_t *s) {
  buffer_output_full_t *bo = (buffer_output_full_t *)s->impl;
  if (!bo || !bo->sb) return -1;
  return (ptrdiff_t)bo->sb->len;
}

static ptrdiff_t bos_file_length(cl_stream_t *s) {
  buffer_output_full_t *bo = (buffer_output_full_t *)s->impl;
  if (!bo || !bo->sb) return -1;
  return (ptrdiff_t)bo->sb->len;
}

cl_stream_t *stream_create_buffer_output(emacs_env *env, emacs_value buffer) {
  buffer_output_full_t *bo = calloc(1, sizeof(*bo));
  if (!bo) return NULL;
  bo->buffer_ref = env->make_global_ref(env, buffer);
  bo->sb = strbuf_create(4096);
  if (!bo->sb) { env->free_global_ref(env, bo->buffer_ref); free(bo); return NULL; }

  cl_stream_t *s = stream_alloc(&buffer_output_ops, bo,
                                CL_STREAM_KIND_BUFFER_OUTPUT, CL_STREAM_FLAG_OUTPUT);
  if (!s) {
    env->free_global_ref(env, bo->buffer_ref);
    strbuf_free(bo->sb); free(bo); return NULL;
  }
  return s;
}

emacs_value Fmake_buffer_output_stream(emacs_env *env, ptrdiff_t nargs,
                                        emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-make-buffer-output-stream requires 1 argument");
    return syms.v_nil;
  }
  cl_stream_t *s = stream_create_buffer_output(env, args[0]);
  return stream_or_error(env, s, "Cannot create buffer output stream");
}

/* ── buffer I/O stream (bidirectional) ────────────────────────────────── */

static ptrdiff_t bds_read(cl_stream_t *s, void *buf, ptrdiff_t n);
static ptrdiff_t bds_write(cl_stream_t *s, const void *buf, ptrdiff_t n);
static ptrdiff_t bds_write_string(emacs_env *env, cl_stream_t *s, emacs_value str);
static int       bds_close(cl_stream_t *s);
static bool      bds_listen(cl_stream_t *s);
static void      bds_destroy(cl_stream_t *s);
static ptrdiff_t bds_file_position(cl_stream_t *s);
static bool      bds_set_file_position(cl_stream_t *s, ptrdiff_t pos);
static ptrdiff_t bds_file_length(cl_stream_t *s);

const cl_stream_ops_t buffer_io_ops = {
  .read         = bds_read,          .write        = bds_write,
  .write_string = bds_write_string,  .close        = bds_close,
  .flush        = NULL,              .finish       = NULL,
  .listen       = bds_listen,        .destroy      = bds_destroy,
  .file_position = bds_file_position,
  .set_file_position = bds_set_file_position,
  .file_length   = bds_file_length,
};

typedef struct {
  char        *data;       /* snapshot at creation */
  size_t       len;
  size_t       pos;        /* shared read/write cursor */
  strbuf_t    *sb;         /* accumulated byte writes */
  emacs_value  buffer_ref; /* global ref */
} buffer_io_full_t;

static ptrdiff_t bds_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  buffer_io_full_t *bi = (buffer_io_full_t *)s->impl;
  if (!bi || !bi->data) return 0;
  return snapshot_read(bi->data, bi->len, &bi->pos, buf, n);
}

static ptrdiff_t bds_write(cl_stream_t *s, const void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  buffer_io_full_t *bi = (buffer_io_full_t *)s->impl;
  if (!bi) return -1;
  if (!strbuf_append(bi->sb, (const char *)buf, n)) return -1;
  bi->pos += (size_t)n;
  return n;
}

static ptrdiff_t bds_write_string(emacs_env *env, cl_stream_t *s, emacs_value str) {
  buffer_io_full_t *bi = (buffer_io_full_t *)s->impl;
  if (!bi) return -1;
  ptrdiff_t written = buffer_write_string(env, bi->buffer_ref, bi->sb, str);
  if (written > 0) bi->pos += (size_t)written;
  return written;
}

static bool bds_listen(cl_stream_t *s) {
  buffer_io_full_t *bi = (buffer_io_full_t *)s->impl;
  return bi && bi->data && bi->pos < bi->len;
}

static int bds_close(cl_stream_t *s) {
  buffer_io_full_t *bi = (buffer_io_full_t *)s->impl;
  if (bi) {
    free(bi->data); bi->data = NULL;
    strbuf_free(bi->sb); bi->sb = NULL;
  }
  atomic_store(&s->open, false);
  return 0;
}

static void bds_destroy(cl_stream_t *s) { free(s->impl); }

static ptrdiff_t bds_file_position(cl_stream_t *s) {
  buffer_io_full_t *bi = (buffer_io_full_t *)s->impl;
  if (!bi) return -1;
  return (ptrdiff_t)bi->pos;
}

static bool bds_set_file_position(cl_stream_t *s, ptrdiff_t pos) {
  buffer_io_full_t *bi = (buffer_io_full_t *)s->impl;
  if (!bi || pos < 0 || (size_t)pos > bi->len) return false;
  bi->pos = (size_t)pos;
  return true;
}

static ptrdiff_t bds_file_length(cl_stream_t *s) {
  buffer_io_full_t *bi = (buffer_io_full_t *)s->impl;
  if (!bi) return -1;
  return (ptrdiff_t)bi->len;
}

cl_stream_t *stream_create_buffer_io(emacs_env *env, emacs_value buffer,
                                     ptrdiff_t start, ptrdiff_t end) {
  size_t slen = 0;
  char *data = buf_snapshot(env, buffer, start, end, &slen);
  if (!data) return NULL;

  buffer_io_full_t *bi = calloc(1, sizeof(*bi));
  if (!bi) { free(data); return NULL; }
  bi->data = data; bi->len = slen; bi->pos = (size_t)start;
  bi->buffer_ref = env->make_global_ref(env, buffer);
  bi->sb = strbuf_create(4096);
  if (!bi->sb) {
    env->free_global_ref(env, bi->buffer_ref);
    free(bi->data); free(bi); return NULL;
  }

  cl_stream_t *s = stream_alloc(&buffer_io_ops, bi, CL_STREAM_KIND_BUFFER_IO,
                                CL_STREAM_FLAG_INPUT | CL_STREAM_FLAG_OUTPUT);
  if (!s) {
    env->free_global_ref(env, bi->buffer_ref);
    strbuf_free(bi->sb); free(bi->data); free(bi); return NULL;
  }
  return s;
}

emacs_value Fmake_buffer_io_stream(emacs_env *env, ptrdiff_t nargs,
                                    emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-make-buffer-io-stream requires 1 argument");
    return syms.v_nil;
  }
  ptrdiff_t start, end;
  buffer_region(env, nargs, args, &start, &end);
  return stream_or_error(env, stream_create_buffer_io(env, args[0], start, end),
                         "Cannot create buffer IO stream");
}

void bds_flush_pending(emacs_env *env, cl_stream_t *s) {
  buffer_io_full_t *bi = (buffer_io_full_t *)s->impl;
  if (bi) buf_flush_strbuf(env, bi->buffer_ref, bi->sb);
}
