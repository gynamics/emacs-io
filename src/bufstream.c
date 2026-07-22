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
  emacs_value buffer_ref;
  ptrdiff_t   pos;
  ptrdiff_t   end;
  char       *chunk;
  size_t      chunk_size;
  size_t      chunk_pos;
  size_t      chunk_len;
} buffer_input_full_t;

cl_stream_t *stream_create_buffer_input(emacs_env *env, emacs_value buffer,
                                        ptrdiff_t start, ptrdiff_t end) {
  buffer_input_full_t *bi = calloc(1, sizeof(*bi));
  if (!bi) return NULL;

  bi->buffer_ref = env->make_global_ref(env, buffer);
  bi->pos = start;
  bi->end = end;
  bi->chunk_size = BUFFER_STREAM_CHUNK;
  bi->chunk = malloc(bi->chunk_size);
  if (!bi->chunk) {
    env->free_global_ref(env, bi->buffer_ref);
    free(bi);
    return NULL;
  }
  bi->chunk_pos = 0;
  bi->chunk_len = 0;

  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) {
    env->free_global_ref(env, bi->buffer_ref);
    free(bi->chunk);
    free(bi);
    return NULL;
  }

  s->ops = &buffer_input_ops;
  s->impl = bi;
  s->flags = CL_STREAM_FLAG_INPUT;
  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);

  return s;
}

static ptrdiff_t fill_chunk(buffer_input_full_t *bi) {
  if (bi->pos >= bi->end) return 0;
  ptrdiff_t remaining = bi->end - bi->pos;
  ptrdiff_t to_read = remaining < (ptrdiff_t)bi->chunk_size
                      ? remaining : (ptrdiff_t)bi->chunk_size;

  emacs_env *env = cl_env();
  if (!env) return 0;

  emacs_value args[2];
  args[0] = eint(env, bi->pos);
  args[1] = eint(env, bi->pos + to_read);
  emacs_value text = efuncall_cached(env, syms.fn_buffer_substring, 2, args);

  ptrdiff_t copied = (ptrdiff_t)bi->chunk_size;
  if (!env->copy_string_contents(env, text, bi->chunk, &copied)) return 0;
  copied -= 1; /* exclude null terminator */

  bi->pos += copied;
  bi->chunk_len = (size_t)copied;
  bi->chunk_pos = 0;
  return copied;
}

static ptrdiff_t bis_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (!bi) return 0;

  ptrdiff_t total = 0;
  unsigned char *dest = (unsigned char *)buf;

  while (total < n) {
    if (bi->chunk_pos >= bi->chunk_len) {
      if (fill_chunk(bi) <= 0) break;
    }
    size_t avail = bi->chunk_len - bi->chunk_pos;
    size_t want = (size_t)(n - total);
    size_t to_copy = avail < want ? avail : want;
    memcpy(dest + total, bi->chunk + bi->chunk_pos, to_copy);
    bi->chunk_pos += to_copy;
    total += (ptrdiff_t)to_copy;
  }
  return total;
}

static int bis_close(cl_stream_t *s) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (bi) {
    emacs_env *env = cl_env();
    if (env) env->free_global_ref(env, bi->buffer_ref);
    free(bi->chunk);
  }
  atomic_store(&s->open, false);
  return 0;
}

static void bis_destroy(cl_stream_t *s) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (bi) {
    free(bi->chunk);
    free(bi);
  }
}

static bool bis_listen(cl_stream_t *s) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (!bi) return false;
  if (bi->chunk_pos < bi->chunk_len) return true;
  return bi->pos < bi->end;
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
    efuncall_cached(env, syms.fn_buffer_size, 1, (emacs_value[]){ args[0] }));

  for (ptrdiff_t i = 1; i < nargs - 1; i += 2) {
    if (kw_eq_cached(env, args[i], syms.k_start))
      start = eint_extract(env, args[i + 1]);
    else if (kw_eq_cached(env, args[i], syms.k_end))
      end = eint_extract(env, args[i + 1]);
  }

  cl_stream_t *s = stream_create_buffer_input(env, args[0], start, end);
  if (!s) {
    signal_error(env, "Cannot create buffer input stream");
    return syms.v_nil;
  }
  return euser_ptr_stream_make(env, s);
}

static ptrdiff_t bis_file_position(cl_stream_t *s) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (!bi) return -1;
  return bi->pos - (ptrdiff_t)bi->chunk_len + (ptrdiff_t)bi->chunk_pos;
}

static bool bis_set_file_position(cl_stream_t *s, ptrdiff_t pos) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (!bi || pos < 0 || pos >= bi->end) return false;
  bi->chunk_pos = 0;
  bi->chunk_len = 0;
  bi->pos = pos;
  return true;
}

static ptrdiff_t bis_file_length(cl_stream_t *s) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (!bi) return -1;
  return bi->end;
}
