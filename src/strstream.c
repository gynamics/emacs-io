#include "stream.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

/* ── string-input ops ────────────────────────────────────────────────── */

static ptrdiff_t sis_read(cl_stream_t *s, void *buf, ptrdiff_t n);
static bool      sis_listen(cl_stream_t *s);
static void      sis_destroy(cl_stream_t *s);
static bool      sis_direct(cl_stream_t *s, const void **addr, size_t *len);
static void      sis_advance(cl_stream_t *s, ptrdiff_t n);
static void      sis_clear_input(cl_stream_t *s);
static ptrdiff_t sis_file_position(cl_stream_t *s);
static bool      sis_set_file_position(cl_stream_t *s, ptrdiff_t pos);
static ptrdiff_t sis_file_length(cl_stream_t *s);

static const cl_stream_ops_t string_input_ops = {
  .read        = sis_read,
  .write       = NULL,
  .close       = NULL,   /* default: atomic store open=false */
  .flush       = NULL,
  .finish      = NULL,
  .listen      = sis_listen,
  .destroy     = sis_destroy,
  .direct      = sis_direct,
  .advance     = sis_advance,
  .clear_input = sis_clear_input,
  .file_position = sis_file_position,
  .set_file_position = sis_set_file_position,
  .file_length    = sis_file_length,
};

/* ── string-output ops ───────────────────────────────────────────────── */

static ptrdiff_t sos_write(cl_stream_t *s, const void *buf, ptrdiff_t n);
static void      sos_destroy(cl_stream_t *s);
static ptrdiff_t sos_write_string(emacs_env *env, cl_stream_t *s, emacs_value str);
static void      sos_clear_output(cl_stream_t *s);
static ptrdiff_t sos_file_position(cl_stream_t *s);
static ptrdiff_t sos_file_length(cl_stream_t *s);

static const cl_stream_ops_t string_output_ops = {
  .read         = NULL,
  .write        = sos_write,
  .close        = NULL,
  .flush        = NULL,
  .finish       = NULL,
  .listen       = NULL,
  .destroy      = sos_destroy,
  .direct       = NULL,
  .advance      = NULL,
  .write_string = sos_write_string,
  .clear_output = sos_clear_output,
  .file_position = sos_file_position,
  .file_length    = sos_file_length,
};

/* ── string input: create / read / close / listen ────────────────────── */

cl_stream_t *stream_create_string_input(const char *data, size_t len) {
  string_input_impl_t *si = malloc(sizeof(*si));
  if (!si) return NULL;
  si->data = data;
  si->len = len;
  si->pos = 0;

  cl_stream_t *s = stream_alloc(&string_input_ops, si, CL_STREAM_FLAG_INPUT);
  if (!s) { free(si); return NULL; }
  s->kind = CL_STREAM_KIND_STRING_INPUT;
  return s;
}

static ptrdiff_t sis_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  if (!si || si->pos >= si->len) return 0; /* EOF */

  size_t remaining = si->len - si->pos;
  size_t to_copy = (size_t)n < remaining ? (size_t)n : remaining;
  memcpy(buf, si->data + si->pos, to_copy);
  si->pos += to_copy;
  return (ptrdiff_t)to_copy;
}

static void sis_destroy(cl_stream_t *s) {
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  if (si) {
    free((void *)si->data);
    free(si);
  }
}

static bool sis_listen(cl_stream_t *s) {
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  return si != NULL && si->pos < si->len;
}

static bool sis_direct(cl_stream_t *s, const void **addr, size_t *len) {
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  if (!si || si->pos >= si->len) return false;
  *addr = si->data + si->pos;
  *len = si->len - si->pos;
  return true;
}

static void sis_advance(cl_stream_t *s, ptrdiff_t n) {
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  if (!si || n <= 0) return;
  si->pos += (size_t)n;
  if (si->pos > si->len) si->pos = si->len;
}

static void sis_clear_input(cl_stream_t *s) {
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  if (si) si->pos = si->len;  /* skip all remaining data */
}

static ptrdiff_t sis_file_position(cl_stream_t *s) {
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  if (!si) return -1;
  return (ptrdiff_t)si->pos;
}

static bool sis_set_file_position(cl_stream_t *s, ptrdiff_t pos) {
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  if (!si || pos < 0 || (size_t)pos > si->len) return false;
  si->pos = (size_t)pos;
  return true;
}

static ptrdiff_t sis_file_length(cl_stream_t *s) {
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  if (!si) return -1;
  return (ptrdiff_t)si->len;
}

/* ── string output: block-chain (no realloc copying) ────────────────────*/

/* Release every block in the chain and reset the accumulator. */
static void sos_free_blocks(str_output_impl_t *so) {
  str_block_t *bk = so->head;
  while (bk) {
    str_block_t *next = bk->next;
    free(bk);
    bk = next;
  }
  so->head = NULL;
  so->tail = NULL;
  so->total_len = 0;
}

/* Append a block that has already been filled with LEN bytes. */
static void sos_link(str_output_impl_t *so, str_block_t *bk, size_t len) {
  bk->len = len;
  if (!so->head) so->head = bk;
  else so->tail->next = bk;
  so->tail = bk;
  so->total_len += len;
}

/* True if the tail block has room for N more bytes. */
static bool sos_tail_fits(const str_output_impl_t *so, size_t n) {
  return so->tail && so->tail->cap - so->tail->len >= n;
}

/* Allocate a block able to hold CAP bytes (flexible array member). */
static str_block_t *str_block_alloc(size_t cap) {
  str_block_t *bk = malloc(sizeof(*bk) + cap);
  if (!bk) return NULL;
  bk->next = NULL;
  bk->len = 0;
  bk->cap = cap;
  return bk;
}

cl_stream_t *stream_create_string_output(size_t hint_size) {
  str_output_impl_t *so = calloc(1, sizeof(*so));
  if (!so) return NULL;

  /* pre-allocate a single block if caller knows the expected size */
  if (hint_size > 0) {
    str_block_t *bk = str_block_alloc(hint_size + 1);  /* +1 for null */
    if (bk) {
      bk->cap = hint_size;  /* user-visible capacity, as before */
      so->head = bk;
      so->tail = bk;
    }
  }

  cl_stream_t *s = stream_alloc(&string_output_ops, so, CL_STREAM_FLAG_OUTPUT);
  if (!s) {
    sos_free_blocks(so);
    free(so);
    return NULL;
  }
  s->kind = CL_STREAM_KIND_STRING_OUTPUT;
  return s;
}

static ptrdiff_t sos_write(cl_stream_t *s, const void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  str_output_impl_t *so = (str_output_impl_t *)s->impl;
  if (!so) return -1;

  /* If tail has room, write directly — zero old-data copying */
  if (sos_tail_fits(so, (size_t)n)) {
    memcpy(so->tail->data + so->tail->len, buf, (size_t)n);
    so->tail->len += (size_t)n;
    so->total_len += (size_t)n;
    return n;
  }

  /* Allocate new block.  For large writes, match size exactly;
     otherwise use default block size. */
  size_t new_cap = STR_BLOCK_DEFAULT_CAP;
  if ((size_t)n > new_cap) new_cap = (size_t)n;

  str_block_t *bk = str_block_alloc(new_cap);
  if (!bk) return -1;
  memcpy(bk->data, buf, (size_t)n);
  sos_link(so, bk, (size_t)n);

  return n;
}

static ptrdiff_t sos_write_string(emacs_env *env, cl_stream_t *s, emacs_value str) {
  str_output_impl_t *so = (str_output_impl_t *)s->impl;
  if (!so) return -1;

  /* query size */
  ptrdiff_t slen = 0;
  if (!env->copy_string_contents(env, str, NULL, &slen)) return -1;
  if (slen <= 1) return 0;

  size_t n = (size_t)(slen - 1);

  /* if tail block has room, write directly into it */
  if (sos_tail_fits(so, n)) {
    ptrdiff_t copied = slen;
    if (!env->copy_string_contents(env, str, so->tail->data + so->tail->len, &copied)) return -1;
    so->tail->len += n;
    so->total_len += n;
    return (ptrdiff_t)n;
  }

  /* allocate new block */
  str_block_t *bk = str_block_alloc(n + 1);
  if (!bk) return -1;

  ptrdiff_t copied = slen;
  if (!env->copy_string_contents(env, str, bk->data, &copied)) {
    free(bk);
    return -1;
  }
  sos_link(so, bk, n);

  return (ptrdiff_t)n;
}

static void sos_clear_output(cl_stream_t *s) {
  str_output_impl_t *so = (str_output_impl_t *)s->impl;
  if (so) sos_free_blocks(so);
}

static void sos_destroy(cl_stream_t *s) {
  str_output_impl_t *so = (str_output_impl_t *)s->impl;
  if (!so) return;
  sos_free_blocks(so);
  free(so);
}

static ptrdiff_t sos_file_position(cl_stream_t *s) {
  str_output_impl_t *so = (str_output_impl_t *)s->impl;
  if (!so) return -1;
  return (ptrdiff_t)so->total_len;
}

static ptrdiff_t sos_file_length(cl_stream_t *s) {
  str_output_impl_t *so = (str_output_impl_t *)s->impl;
  if (!so) return -1;
  return (ptrdiff_t)so->total_len;
}

/* ── get accumulated string from block-chain output stream ───────────── */

emacs_value str_output_to_string(emacs_env *env, cl_stream_t *s) {
  str_output_impl_t *so = (str_output_impl_t *)s->impl;
  if (!so) return syms.v_nil;

  if (!so->head) return env->make_string(env, "", 0);

  /* Single block — return directly */
  if (so->head == so->tail)
    return env->make_string(env, so->head->data, (ptrdiff_t)so->head->len);

  /* Multiple blocks — use Emacs concat (uses Emacs' own allocator) */
  int nblocks = 0;
  for (str_block_t *bk = so->head; bk; bk = bk->next) nblocks++;

  emacs_value *elts = malloc((size_t)nblocks * sizeof(emacs_value));
  if (!elts) return syms.v_nil;
  str_block_t *bk = so->head;
  for (int i = 0; i < nblocks; i++, bk = bk->next) {
    if (!bk) break;
    elts[i] = env->make_string(env, bk->data, (ptrdiff_t)bk->len);
  }

  emacs_value concat_sym = eintern(env, "concat");
  emacs_value result = env->funcall(env, concat_sym, nblocks, elts);
  free(elts);
  return result;
}

/* ── cl-make-string-input-stream ──────────────────────────────────────── */

emacs_value Fmake_string_input(emacs_env *env, ptrdiff_t nargs,
                               emacs_value *args, void *data) {
  (void)data;
  /* (cl-make-string-input-stream string &optional start end) */
  if (nargs < 1) {
    signal_error(env, "cl-make-string-input-stream requires 1 argument");
    return syms.v_nil;
  }

  ptrdiff_t slen = 0;
  char *str = estring_alloc(env, args[0], &slen);
  if (!str) {
    signal_error(env, "Cannot extract string");
    return syms.v_nil;
  }

  ptrdiff_t start = 0;
  ptrdiff_t end = slen;
  if (nargs >= 2 && env->is_not_nil(env, args[1]))
    start = eint_extract(env, args[1]);
  if (nargs >= 3 && env->is_not_nil(env, args[2]))
    end = eint_extract(env, args[2]);

  if (start < 0) start = 0;
  if (end > slen) end = slen;
  if (end < start) end = start;

  /* pass the full malloc'd buffer; set cursor via impl fields.
     sis_destroy will free(str) — must be the base of the allocation. */
  cl_stream_t *s = stream_create_string_input(str, (size_t)slen);
  if (!s) {
    free(str);
    signal_error(env, "Cannot create string input stream");
    return syms.v_nil;
  }
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  si->pos = (size_t)start;
  si->len = (size_t)end;

  return euser_ptr_stream_make(env, s);
}

/* ── cl-make-string-output-stream ──────────────────────────────────────── */

emacs_value Fmake_string_output(emacs_env *env, ptrdiff_t nargs,
                                emacs_value *args, void *data) {
  (void)data;
  /* (cl-make-string-output-stream &key element-type size) */
  size_t hint = 0;
  for (ptrdiff_t i = 0; i < nargs - 1; i += 2) {
    if (kw_eq_cached(env, args[i], syms.k_size))
      hint = (size_t)eint_extract(env, args[i + 1]);
    else if (kw_eq_cached(env, args[i], syms.k_element_type)) {
      if (!env->eq(env, args[i + 1], syms.v_character)) {
        signal_error(env, "string-output-stream: only character element type supported");
        return syms.v_nil;
      }
    }
  }
  return stream_or_error(env, stream_create_string_output(hint),
                         "Cannot create string output stream");
}

/* ── cl-get-output-stream-string ──────────────────────────────────────── */

emacs_value Fget_output_stream_string(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-get-output-stream-string requires 1 argument");
    return syms.v_nil;
  }
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s || s->kind != CL_STREAM_KIND_STRING_OUTPUT) {
    signal_error(env, "Not a string output stream");
    return syms.v_nil;
  }
  return str_output_to_string(env, s);
}
