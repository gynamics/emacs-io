#include "stream.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

/* ── buffer input stream ─────────────────────────────────────────────── */

static ptrdiff_t bis_read(cl_stream_t *s, void *buf, ptrdiff_t n);
static int       bis_close(cl_stream_t *s);
static bool      bis_listen(cl_stream_t *s);
static void      bis_destroy(cl_stream_t *s);
static ptrdiff_t bis_file_position(cl_stream_t *s);
static bool      bis_set_file_position(cl_stream_t *s, ptrdiff_t pos);
static ptrdiff_t bis_file_length(cl_stream_t *s);

static const cl_stream_ops_t buffer_input_ops = {
  .read    = bis_read,
  .write   = NULL,
  .close   = bis_close,
  .flush   = NULL,
  .finish  = NULL,
  .listen  = bis_listen,
  .destroy = bis_destroy,
  .file_position = bis_file_position,
  .set_file_position = bis_set_file_position,
  .file_length    = bis_file_length,
};

typedef struct {
  char       *data;
  size_t      len;
  size_t      pos;
} buffer_input_full_t;

cl_stream_t *stream_create_buffer_input(emacs_env *env, emacs_value buffer,
                                        ptrdiff_t start, ptrdiff_t end) {
  /* snapshot the buffer region — save/restore current buffer */
  emacs_value old = efuncall(env, "current-buffer", 0, NULL);
  efuncall(env, "set-buffer", 1, &buffer);
  emacs_value args[2];
  args[0] = eint(env, start + 1);   /* Emacs positions are 1-indexed */
  args[1] = eint(env, end + 1);
  emacs_value text = efuncall_cached(env, syms.fn_buffer_substring, 2, args);
  efuncall(env, "set-buffer", 1, &old);

  ptrdiff_t slen = 0;
  if (!env->copy_string_contents(env, text, NULL, &slen)) return NULL;
  if (slen <= 0) return NULL;

  char *data = malloc((size_t)slen);
  if (!data) return NULL;
  ptrdiff_t copied = slen;
  if (!env->copy_string_contents(env, text, data, &copied)) {
    free(data);
    return NULL;
  }
  copied -= 1;   /* exclude null terminator */

  buffer_input_full_t *bi = calloc(1, sizeof(*bi));
  if (!bi) { free(data); return NULL; }

  bi->data = data;
  bi->len = (size_t)copied;
  bi->pos = 0;

  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) {
    free(bi->data);
    free(bi);
    return NULL;
  }

  s->ops = &buffer_input_ops;
  s->impl = bi;
  s->kind = CL_STREAM_KIND_BUFFER_INPUT;
  s->flags = CL_STREAM_FLAG_INPUT;
  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);

  return s;
}

static ptrdiff_t bis_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (!bi || bi->pos >= bi->len) return 0;

  size_t remaining = bi->len - bi->pos;
  size_t to_copy = (size_t)n < remaining ? (size_t)n : remaining;
  memcpy(buf, bi->data + bi->pos, to_copy);
  bi->pos += to_copy;
  return (ptrdiff_t)to_copy;
}

static int bis_close(cl_stream_t *s) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (bi) {
    free(bi->data);
    bi->data = NULL;
    bi->len = 0;
    bi->pos = 0;
  }
  atomic_store(&s->open, false);
  return 0;
}

static void bis_destroy(cl_stream_t *s) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (bi) {
    free(bi);
  }
}

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

/* ── cl-make-buffer-input-stream ────────────────────────────────────────
   (cl-make-buffer-input-stream buffer &key start end) */

emacs_value Fmake_buffer_input_stream(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-make-buffer-input-stream requires 1 argument (buffer)");
    return syms.v_nil;
  }

  ptrdiff_t start = 0;
  ptrdiff_t end = eint_extract(env,
    efuncall_cached(env, syms.fn_buffer_size, 1, &args[0]));

  for (ptrdiff_t i = 1; i < nargs - 1; i += 2) {
    if (kw_eq_cached(env, args[i], syms.k_start))
      start = eint_extract(env, args[i + 1]);
    else if (kw_eq_cached(env, args[i], syms.k_end))
      end = eint_extract(env, args[i + 1]);
  }

  if (start < 0) start = 0;
  if (end < start) end = start;

  cl_stream_t *s = stream_create_buffer_input(env, args[0], start, end);
  if (!s) {
    signal_error(env, "Cannot create buffer input stream");
    return syms.v_nil;
  }
  return euser_ptr_stream_make(env, s);
}

/* ── buffer output stream ────────────────────────────────────────────── */

static ptrdiff_t bos_write(cl_stream_t *s, const void *buf, ptrdiff_t n);
static ptrdiff_t bos_write_string(emacs_env *env, cl_stream_t *s, emacs_value str);
static int       bos_close(cl_stream_t *s);
static void      bos_destroy(cl_stream_t *s);
static void      bos_clear_output(cl_stream_t *s);
static ptrdiff_t bos_file_position(cl_stream_t *s);
static ptrdiff_t bos_file_length(cl_stream_t *s);

static const cl_stream_ops_t buffer_output_ops = {
  .read         = NULL,
  .write        = bos_write,
  .write_string = bos_write_string,
  .close        = bos_close,
  .flush        = NULL,
  .finish       = NULL,
  .listen       = NULL,
  .destroy      = bos_destroy,
  .clear_output = bos_clear_output,
  .file_position = bos_file_position,
  .file_length   = bos_file_length,
};

typedef struct {
  emacs_value buffer_ref;   /* global ref to target buffer */
  strbuf_t   *sb;            /* accumulated byte writes */
} buffer_output_full_t;

static void bos_insert(emacs_env *env, buffer_output_full_t *bo,
                        const char *data, ptrdiff_t len) {
  if (!bo || !bo->buffer_ref || !env || len <= 0) return;
  /* switch to target buffer, insert data, restore */
  emacs_value old = efuncall(env, "current-buffer", 0, NULL);
  efuncall(env, "set-buffer", 1, &bo->buffer_ref);
  emacs_value str = env->make_string(env, data, len);
  efuncall(env, "insert", 1, &str);
  efuncall(env, "set-buffer", 1, &old);
}

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
  /* flush any accumulated byte writes first */
  if (bo->sb->len > 0) {
    bos_insert(env, bo, bo->sb->data, (ptrdiff_t)bo->sb->len);
    bo->sb->len = 0;
    bo->sb->data[0] = '\0';
  }
  /* extract the string and insert to buffer */
  ptrdiff_t slen = 0;
  char *cstr = estring_alloc(env, str, &slen);
  if (!cstr) return -1;
  bos_insert(env, bo, cstr, slen);
  free(cstr);
  return slen;
}

static int bos_close(cl_stream_t *s) {
  buffer_output_full_t *bo = (buffer_output_full_t *)s->impl;
  if (bo) {
    strbuf_free(bo->sb);
    bo->sb = NULL;
  }
  atomic_store(&s->open, false);
  return 0;
}

static void bos_destroy(cl_stream_t *s) {
  buffer_output_full_t *bo = (buffer_output_full_t *)s->impl;
  if (bo) free(bo);
}

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
  if (!bo->sb) {
    env->free_global_ref(env, bo->buffer_ref);
    free(bo);
    return NULL;
  }

  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) {
    env->free_global_ref(env, bo->buffer_ref);
    strbuf_free(bo->sb);
    free(bo);
    return NULL;
  }

  s->ops = &buffer_output_ops;
  s->impl = bo;
  s->flags = CL_STREAM_FLAG_OUTPUT;
  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);

  return s;
}

/* ── cl-make-buffer-output-stream ─────────────────────────────────── */

emacs_value Fmake_buffer_output_stream(emacs_env *env, ptrdiff_t nargs,
                                        emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-make-buffer-output-stream requires 1 argument (buffer)");
    return syms.v_nil;
  }

  cl_stream_t *s = stream_create_buffer_output(env, args[0]);
  if (!s) {
    signal_error(env, "Cannot create buffer output stream");
    return syms.v_nil;
  }
  return euser_ptr_stream_make(env, s);
}
