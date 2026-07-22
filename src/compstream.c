#define _POSIX_C_SOURCE 200809L
#include "stream.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

/* ── synonym stream ────────────────────────────────────────────────────
   The target stream is resolved once at construction using the
   constructor's valid emacs_env, then cached.  The vtable ops
   forward to the cached target.  Re-resolution on each I/O (per CL
   spec) would require calling Emacs Lisp outside the mutex, which
   is not currently supported by the dispatch layer. */

typedef struct {
  cl_stream_t  *cached;
  emacs_value   symbol;    /* global-ref'd symbol, for accessor */
} synonym_impl_t;

static ptrdiff_t syn_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  cl_stream_t *t = ((synonym_impl_t *)s->impl)->cached;
  if (!t || !stream_is_open(t)) return 0;
  return stream_read(t, buf, n);
}

static ptrdiff_t syn_write(cl_stream_t *s, const void *buf, ptrdiff_t n) {
  cl_stream_t *t = ((synonym_impl_t *)s->impl)->cached;
  if (!t || !stream_is_open(t)) return 0;
  return stream_write(t, buf, n);
}

static bool syn_listen(cl_stream_t *s) {
  cl_stream_t *t = ((synonym_impl_t *)s->impl)->cached;
  if (!t) return false;
  return stream_listen(t);
}

static int syn_close(cl_stream_t *s) {
  atomic_store(&s->open, false);
  return 0;
}

static void syn_destroy(cl_stream_t *s) {
  /* symbol is permanent, no need to free_global_ref */
  free(s->impl);
}

static ptrdiff_t syn_file_position(cl_stream_t *s) {
  cl_stream_t *t = ((synonym_impl_t *)s->impl)->cached;
  if (!t) return -1;
  return stream_file_position(t);
}

static bool syn_set_file_position(cl_stream_t *s, ptrdiff_t pos) {
  cl_stream_t *t = ((synonym_impl_t *)s->impl)->cached;
  if (!t) return false;
  return stream_set_file_position(t, pos);
}

static ptrdiff_t syn_file_length(cl_stream_t *s) {
  cl_stream_t *t = ((synonym_impl_t *)s->impl)->cached;
  if (!t) return -1;
  return stream_file_length(t);
}

static const cl_stream_ops_t synonym_stream_ops = {
  .read          = syn_read,
  .write         = syn_write,
  .close         = syn_close,
  .flush         = NULL,   /* synonym: delegate on next write */
  .finish        = NULL,
  .listen        = syn_listen,
  .destroy       = syn_destroy,
  .file_position = syn_file_position,
  .set_file_position = syn_set_file_position,
  .file_length   = syn_file_length,
};

cl_stream_t *stream_create_synonym(cl_stream_t *target, emacs_env *env,
                                    emacs_value symbol) {
  synonym_impl_t *si = calloc(1, sizeof(*si));
  if (!si) return NULL;
  si->cached = target;
  if (env && env->is_not_nil(env, symbol))
    si->symbol = env->make_global_ref(env, symbol);

  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) { free(si); return NULL; }

  s->ops = &synonym_stream_ops;
  s->impl = si;
  s->flags = CL_STREAM_FLAG_INPUT | CL_STREAM_FLAG_OUTPUT;
  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);
  return s;
}

/* ── broadcast stream ──────────────────────────────────────────────────
   Writes to all sub-streams.  Reads/listens/closes are forwarded to
   the first sub-stream (convention). */

typedef struct {
  cl_stream_t **streams;
  ptrdiff_t     count;
} broadcast_impl_t;

static ptrdiff_t bcast_write(cl_stream_t *s, const void *buf, ptrdiff_t n) {
  broadcast_impl_t *bc = (broadcast_impl_t *)s->impl;
  if (!bc) return -1;
  ptrdiff_t last = n;
  for (ptrdiff_t i = 0; i < bc->count; i++) {
    last = stream_write(bc->streams[i], buf, n);
    if (last < 0) return last;
  }
  return last;
}

static ptrdiff_t bcast_write_string(emacs_env *env, cl_stream_t *s,
                                     emacs_value str) {
  broadcast_impl_t *bc = (broadcast_impl_t *)s->impl;
  if (!bc) return -1;
  ptrdiff_t last = 0;
  for (ptrdiff_t i = 0; i < bc->count; i++) {
    last = stream_write_string(env, bc->streams[i], str);
    if (last < 0) return last;
  }
  return last;
}

static int bcast_close(cl_stream_t *s) {
  broadcast_impl_t *bc = (broadcast_impl_t *)s->impl;
  if (bc) {
    for (ptrdiff_t i = 0; i < bc->count; i++)
      stream_close(bc->streams[i]);
  }
  atomic_store(&s->open, false);
  return 0;
}

static void bcast_destroy(cl_stream_t *s) {
  broadcast_impl_t *bc = (broadcast_impl_t *)s->impl;
  if (bc) { free(bc->streams); free(bc); }
}

static void bcast_clear_output(cl_stream_t *s) {
  broadcast_impl_t *bc = (broadcast_impl_t *)s->impl;
  if (!bc) return;
  for (ptrdiff_t i = 0; i < bc->count; i++)
    if (bc->streams[i]->ops && bc->streams[i]->ops->clear_output)
      bc->streams[i]->ops->clear_output(bc->streams[i]);
}

static int bcast_flush(cl_stream_t *s) {
  broadcast_impl_t *bc = (broadcast_impl_t *)s->impl;
  int r = 0;
  if (bc) {
    for (ptrdiff_t i = 0; i < bc->count; i++)
      r = stream_flush(bc->streams[i]);
  }
  return r;
}

static int bcast_finish(cl_stream_t *s) {
  broadcast_impl_t *bc = (broadcast_impl_t *)s->impl;
  int r = 0;
  if (bc) {
    for (ptrdiff_t i = 0; i < bc->count; i++)
      r = stream_finish(bc->streams[i]);
  }
  return r;
}

static const cl_stream_ops_t broadcast_stream_ops = {
  .write        = bcast_write,
  .close        = bcast_close,
  .flush        = bcast_flush,
  .finish       = bcast_finish,
  .destroy      = bcast_destroy,
  .write_string = bcast_write_string,
  .clear_output = bcast_clear_output,
};

/* ── concatenated stream ───────────────────────────────────────────────
   Reads from sub-streams in sequence, advancing when one hits EOF. */

typedef struct {
  cl_stream_t **streams;
  ptrdiff_t     count;
  ptrdiff_t     current;   /* index of active sub-stream */
} concat_impl_t;

static ptrdiff_t concat_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  concat_impl_t *ci = (concat_impl_t *)s->impl;
  if (!ci) return 0;
  while (ci->current < ci->count) {
    ptrdiff_t r = stream_read(ci->streams[ci->current], buf, n);
    if (r > 0) return r;
    if (r < 0) return r;
    ci->current++;
  }
  return 0;  /* all streams exhausted */
}

static bool concat_listen(cl_stream_t *s) {
  concat_impl_t *ci = (concat_impl_t *)s->impl;
  if (!ci) return false;
  while (ci->current < ci->count) {
    if (stream_listen(ci->streams[ci->current])) return true;
    ci->current++;
  }
  return false;
}

static int concat_close(cl_stream_t *s) {
  concat_impl_t *ci = (concat_impl_t *)s->impl;
  if (ci) {
    for (ptrdiff_t i = 0; i < ci->count; i++)
      stream_close(ci->streams[i]);
  }
  atomic_store(&s->open, false);
  return 0;
}

static void concat_destroy(cl_stream_t *s) {
  concat_impl_t *ci = (concat_impl_t *)s->impl;
  if (ci) { free(ci->streams); free(ci); }
}

static const cl_stream_ops_t concat_stream_ops = {
  .read    = concat_read,
  .close   = concat_close,
  .listen  = concat_listen,
  .destroy = concat_destroy,
};

/* ── constructors ────────────────────────────────────────────────────── */

cl_stream_t *stream_create_broadcast(cl_stream_t **streams, ptrdiff_t count) {
  broadcast_impl_t *bc = calloc(1, sizeof(*bc));
  if (!bc) return NULL;
  bc->streams = malloc((size_t)count * sizeof(cl_stream_t *));
  if (!bc->streams) { free(bc); return NULL; }
  memcpy(bc->streams, streams, (size_t)count * sizeof(cl_stream_t *));
  bc->count = count;

  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) { free(bc->streams); free(bc); return NULL; }
  s->ops = &broadcast_stream_ops;
  s->impl = bc;
  s->flags = CL_STREAM_FLAG_OUTPUT;
  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);
  return s;
}

cl_stream_t *stream_create_concatenated(cl_stream_t **streams, ptrdiff_t count) {
  concat_impl_t *ci = calloc(1, sizeof(*ci));
  if (!ci) return NULL;
  ci->streams = malloc((size_t)count * sizeof(cl_stream_t *));
  if (!ci->streams) { free(ci); return NULL; }
  memcpy(ci->streams, streams, (size_t)count * sizeof(cl_stream_t *));
  ci->count = count;
  ci->current = 0;

  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) { free(ci->streams); free(ci); return NULL; }
  s->ops = &concat_stream_ops;
  s->impl = ci;
  s->flags = CL_STREAM_FLAG_INPUT;
  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);
  return s;
}

/* ── Lisp wrappers ────────────────────────────────────────────────────── */

emacs_value Fmake_synonym_stream(emacs_env *env, ptrdiff_t nargs,
                                  emacs_value *args, void *data) {
  (void)data;
  if (nargs < 1) {
    signal_error(env, "cl-make-synonym-stream requires a symbol");
    return syms.v_nil;
  }
  /* look up the symbol's dynamic value to get the target stream */
  emacs_value target_val = efuncall(env, "symbol-value", 1, &args[0]);
  cl_stream_t *target = euser_ptr_stream_get(env, target_val);
  if (!target) {
    signal_error(env, "Synonym target is not a stream");
    return syms.v_nil;
  }
  cl_stream_t *s = stream_create_synonym(target, env, args[0]);
  if (!s) { signal_error(env, "Cannot create synonym stream"); return syms.v_nil; }
  return euser_ptr_stream_make(env, s);
}

emacs_value Fsynonym_stream_symbol(emacs_env *env, ptrdiff_t nargs,
                                    emacs_value *args, void *data) {
  (void)nargs; (void)data;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s || s->ops != &synonym_stream_ops)
    { signal_error(env, "Not a synonym stream"); return syms.v_nil; }
  synonym_impl_t *si = (synonym_impl_t *)s->impl;
  return si->symbol ? si->symbol : syms.v_nil;
}

emacs_value Fmake_broadcast_stream(emacs_env *env, ptrdiff_t nargs,
                                    emacs_value *args, void *data) {
  (void)data;
  cl_stream_t **list = malloc((size_t)nargs * sizeof(cl_stream_t *));
  if (!list && nargs > 0) { signal_error(env, "Memory allocation failed"); return syms.v_nil; }
  for (ptrdiff_t i = 0; i < nargs; i++) {
    list[i] = euser_ptr_stream_get(env, args[i]);
    if (!list[i]) { free(list); signal_error(env, "Invalid stream"); return syms.v_nil; }
  }
  cl_stream_t *s = stream_create_broadcast(list, nargs);
  free(list);
  if (!s) { signal_error(env, "Cannot create broadcast stream"); return syms.v_nil; }
  return euser_ptr_stream_make(env, s);
}

emacs_value Fmake_concatenated_stream(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value *args, void *data) {
  (void)data;
  cl_stream_t **list = malloc((size_t)nargs * sizeof(cl_stream_t *));
  if (!list && nargs > 0) { signal_error(env, "Memory allocation failed"); return syms.v_nil; }
  for (ptrdiff_t i = 0; i < nargs; i++) {
    list[i] = euser_ptr_stream_get(env, args[i]);
    if (!list[i]) { free(list); signal_error(env, "Invalid stream"); return syms.v_nil; }
  }
  cl_stream_t *s = stream_create_concatenated(list, nargs);
  free(list);
  if (!s) { signal_error(env, "Cannot create concatenated stream"); return syms.v_nil; }
  return euser_ptr_stream_make(env, s);
}

emacs_value Fbroadcast_stream_streams(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s || s->ops != &broadcast_stream_ops)
    { signal_error(env, "Not a broadcast stream"); return syms.v_nil; }
  broadcast_impl_t *bc = (broadcast_impl_t *)s->impl;
  emacs_value lst = syms.v_nil;
  for (ptrdiff_t i = bc->count - 1; i >= 0; i--) {
    emacs_value pair[2] = { euser_ptr_stream_make(env, bc->streams[i]), lst };
    lst = efuncall_cached(env, syms.fn_cons, 2, pair);
  }
  return lst;
}

emacs_value Fconcatenated_stream_streams(emacs_env *env, ptrdiff_t nargs,
                                          emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s || s->ops != &concat_stream_ops)
    { signal_error(env, "Not a concatenated stream"); return syms.v_nil; }
  concat_impl_t *ci = (concat_impl_t *)s->impl;
  emacs_value lst = syms.v_nil;
  for (ptrdiff_t i = ci->count - 1; i >= 0; i--) {
    emacs_value pair[2] = { euser_ptr_stream_make(env, ci->streams[i]), lst };
    lst = efuncall_cached(env, syms.fn_cons, 2, pair);
  }
  return lst;
}

/* ── two-way stream ──────────────────────────────────────────────────── */

typedef struct {
  cl_stream_t *input;
  cl_stream_t *output;
} twoway_impl_t;

static ptrdiff_t tw_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  cl_stream_t *in = ((twoway_impl_t *)s->impl)->input;
  if (!in || !stream_is_open(in)) return 0;
  return stream_read(in, buf, n);
}
static ptrdiff_t tw_write(cl_stream_t *s, const void *buf, ptrdiff_t n) {
  cl_stream_t *out = ((twoway_impl_t *)s->impl)->output;
  if (!out || !stream_is_open(out)) return 0;
  return stream_write(out, buf, n);
}
static bool tw_listen(cl_stream_t *s) {
  cl_stream_t *in = ((twoway_impl_t *)s->impl)->input;
  return in && stream_listen(in);
}
static int tw_close(cl_stream_t *s) {
  twoway_impl_t *tw = (twoway_impl_t *)s->impl;
  if (tw->input)  stream_close(tw->input);
  if (tw->output) stream_close(tw->output);
  atomic_store(&s->open, false);
  return 0;
}
static void tw_destroy(cl_stream_t *s) {
  free(s->impl);
}
static int tw_flush(cl_stream_t *s) {
  cl_stream_t *out = ((twoway_impl_t *)s->impl)->output;
  return out ? stream_flush(out) : 0;
}
static int tw_finish(cl_stream_t *s) {
  cl_stream_t *out = ((twoway_impl_t *)s->impl)->output;
  return out ? stream_finish(out) : 0;
}
static void tw_clear_output(cl_stream_t *s) {
  cl_stream_t *out = ((twoway_impl_t *)s->impl)->output;
  if (out && out->ops && out->ops->clear_output)
    out->ops->clear_output(out);
}
static void tw_clear_input(cl_stream_t *s) {
  cl_stream_t *in = ((twoway_impl_t *)s->impl)->input;
  if (in && in->ops && in->ops->clear_input)
    in->ops->clear_input(in);
}
static ptrdiff_t tw_write_string(emacs_env *env, cl_stream_t *s, emacs_value str) {
  cl_stream_t *out = ((twoway_impl_t *)s->impl)->output;
  if (!out || !stream_is_open(out)) return 0;
  return stream_write_string(env, out, str);
}

static const cl_stream_ops_t twoway_stream_ops = {
  .read    = tw_read,   .write   = tw_write,
  .listen  = tw_listen, .close   = tw_close,
  .destroy = tw_destroy,
  .flush   = tw_flush,  .finish  = tw_finish,
  .clear_output = tw_clear_output, .clear_input = tw_clear_input,
  .write_string  = tw_write_string,
};

/* ── echo stream ─────────────────────────────────────────────────────── */

typedef struct {
  cl_stream_t *input;
  cl_stream_t *output;
} echo_impl_t;

static ptrdiff_t echo_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  echo_impl_t *ec = (echo_impl_t *)s->impl;
  if (!ec->input || !stream_is_open(ec->input)) return 0;
  ptrdiff_t r = stream_read(ec->input, buf, n);
  if (r > 0 && ec->output && stream_is_open(ec->output))
    stream_write(ec->output, buf, r);
  return r;
}
static ptrdiff_t echo_write(cl_stream_t *s, const void *buf, ptrdiff_t n) {
  cl_stream_t *out = ((echo_impl_t *)s->impl)->output;
  if (!out || !stream_is_open(out)) return 0;
  return stream_write(out, buf, n);
}
static bool echo_listen(cl_stream_t *s) {
  cl_stream_t *in = ((echo_impl_t *)s->impl)->input;
  return in && stream_listen(in);
}
static int echo_close(cl_stream_t *s) {
  echo_impl_t *ec = (echo_impl_t *)s->impl;
  if (ec->input)  stream_close(ec->input);
  if (ec->output) stream_close(ec->output);
  atomic_store(&s->open, false);
  return 0;
}
static void echo_destroy(cl_stream_t *s) { free(s->impl); }
static int echo_flush(cl_stream_t *s) {
  cl_stream_t *out = ((echo_impl_t *)s->impl)->output;
  return out ? stream_flush(out) : 0;
}
static void echo_clear_output(cl_stream_t *s) {
  cl_stream_t *out = ((echo_impl_t *)s->impl)->output;
  if (out && out->ops && out->ops->clear_output)
    out->ops->clear_output(out);
}
static void echo_clear_input(cl_stream_t *s) {
  cl_stream_t *in = ((echo_impl_t *)s->impl)->input;
  if (in && in->ops && in->ops->clear_input)
    in->ops->clear_input(in);
}
static ptrdiff_t echo_write_string(emacs_env *env, cl_stream_t *s, emacs_value str) {
  cl_stream_t *out = ((echo_impl_t *)s->impl)->output;
  if (!out || !stream_is_open(out)) return 0;
  return stream_write_string(env, out, str);
}

static const cl_stream_ops_t echo_stream_ops = {
  .read    = echo_read,  .write   = echo_write,
  .listen  = echo_listen,.close   = echo_close,
  .destroy = echo_destroy,
  .flush   = echo_flush,
  .clear_output = echo_clear_output, .clear_input = echo_clear_input,
  .write_string  = echo_write_string,
};

/* ── constructors ────────────────────────────────────────────────────── */

cl_stream_t *stream_create_twoway(cl_stream_t *input, cl_stream_t *output) {
  twoway_impl_t *tw = calloc(1, sizeof(*tw));
  if (!tw) return NULL;
  tw->input = input; tw->output = output;
  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) { free(tw); return NULL; }
  s->ops = &twoway_stream_ops; s->impl = tw;
  s->flags = CL_STREAM_FLAG_INPUT | CL_STREAM_FLAG_OUTPUT;
  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);
  return s;
}

cl_stream_t *stream_create_echo(cl_stream_t *input, cl_stream_t *output) {
  echo_impl_t *ec = calloc(1, sizeof(*ec));
  if (!ec) return NULL;
  ec->input = input; ec->output = output;
  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) { free(ec); return NULL; }
  s->ops = &echo_stream_ops; s->impl = ec;
  s->flags = CL_STREAM_FLAG_INPUT | CL_STREAM_FLAG_OUTPUT;
  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);
  return s;
}

/* ── Lisp wrappers ────────────────────────────────────────────────────── */

emacs_value Fmake_two_way_stream(emacs_env *env, ptrdiff_t nargs,
                                  emacs_value *args, void *data) {
  (void)data;
  if (nargs < 2) {
    signal_error(env, "cl-make-two-way-stream requires 2 arguments");
    return syms.v_nil;
  }
  cl_stream_t *in  = euser_ptr_stream_get(env, args[0]);
  cl_stream_t *out = euser_ptr_stream_get(env, args[1]);
  if (!in || !out) { signal_error(env, "Invalid stream"); return syms.v_nil; }
  cl_stream_t *s = stream_create_twoway(in, out);
  if (!s) { signal_error(env, "Cannot create two-way stream"); return syms.v_nil; }
  return euser_ptr_stream_make(env, s);
}

emacs_value Ftwo_way_stream_input_stream(emacs_env *env, ptrdiff_t nargs,
                                          emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s || s->ops != &twoway_stream_ops)
    { signal_error(env, "Not a two-way stream"); return syms.v_nil; }
  return euser_ptr_stream_make(env, ((twoway_impl_t *)s->impl)->input);
}

emacs_value Ftwo_way_stream_output_stream(emacs_env *env, ptrdiff_t nargs,
                                           emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s || s->ops != &twoway_stream_ops)
    { signal_error(env, "Not a two-way stream"); return syms.v_nil; }
  return euser_ptr_stream_make(env, ((twoway_impl_t *)s->impl)->output);
}

emacs_value Fmake_echo_stream(emacs_env *env, ptrdiff_t nargs,
                               emacs_value *args, void *data) {
  (void)data;
  if (nargs < 2) {
    signal_error(env, "cl-make-echo-stream requires 2 arguments");
    return syms.v_nil;
  }
  cl_stream_t *in  = euser_ptr_stream_get(env, args[0]);
  cl_stream_t *out = euser_ptr_stream_get(env, args[1]);
  if (!in || !out) { signal_error(env, "Invalid stream"); return syms.v_nil; }
  cl_stream_t *s = stream_create_echo(in, out);
  if (!s) { signal_error(env, "Cannot create echo stream"); return syms.v_nil; }
  return euser_ptr_stream_make(env, s);
}

emacs_value Fecho_stream_input_stream(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s || s->ops != &echo_stream_ops)
    { signal_error(env, "Not an echo stream"); return syms.v_nil; }
  return euser_ptr_stream_make(env, ((echo_impl_t *)s->impl)->input);
}

emacs_value Fecho_stream_output_stream(emacs_env *env, ptrdiff_t nargs,
                                        emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = euser_ptr_stream_get(env, args[0]);
  if (!s || s->ops != &echo_stream_ops)
    { signal_error(env, "Not an echo stream"); return syms.v_nil; }
  return euser_ptr_stream_make(env, ((echo_impl_t *)s->impl)->output);
}
