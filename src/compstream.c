#define _POSIX_C_SOURCE 200809L
#include "stream.h"
#include "util.h"
#include "subrs.h"
#include <stdlib.h>
#include <string.h>

/* ── shared helpers ──────────────────────────────────────────────────── */

/* Return the stream behind OBJ, or signal WHAT and return NULL. */
static cl_stream_t *require_stream(emacs_env *env, emacs_value obj,
                                   const char *what) {
  cl_stream_t *s = euser_ptr_stream_get(env, obj);
  if (!s) { signal_error(env, what); return NULL; }
  return s;
}

/* Return the stream behind OBJ if its vtable is OPS, else signal WHAT. */
static cl_stream_t *require_stream_of(emacs_env *env, emacs_value obj,
                                      const cl_stream_ops_t *ops,
                                      const char *what) {
  cl_stream_t *s = euser_ptr_stream_get(env, obj);
  if (!s || s->ops != ops) { signal_error(env, what); return NULL; }
  return s;
}

/* Build an array of the streams named by ARGS (NARGS of them).  Signals
   and returns NULL if any argument is not a stream. */
static cl_stream_t **collect_streams(emacs_env *env, ptrdiff_t nargs,
                                     emacs_value *args) {
  ptrdiff_t cap = nargs > 0 ? nargs : 1;   /* never return NULL on success */
  cl_stream_t **list = malloc((size_t)cap * sizeof(*list));
  if (!list) { signal_error(env, "Memory allocation failed"); return NULL; }
  for (ptrdiff_t i = 0; i < nargs; i++) {
    list[i] = euser_ptr_stream_get(env, args[i]);
    if (!list[i]) { free(list); signal_error(env, "Invalid stream"); return NULL; }
  }
  return list;
}

/* Build a Lisp list of the COUNT streams in STREAMS. */
static emacs_value streams_to_list(emacs_env *env, cl_stream_t **streams,
                                   ptrdiff_t count) {
  emacs_value lst = syms.v_nil;
  for (ptrdiff_t i = count - 1; i >= 0; i--) {
    emacs_value pair[2] = { euser_ptr_stream_ref(env, streams[i]), lst };
    lst = efuncall_cached(env, syms.fn_cons, 2, pair);
  }
  return lst;
}

/* ── synonym stream ────────────────────────────────────────────────────
   The target stream is re-resolved from the symbol's current value before
   each operation, via the vtable `resolve' hook that the dispatch layer
   calls (outside the stream mutex).  This matches CL, where rebinding the
   symbol is reflected immediately. */

typedef struct {
  cl_stream_t  *cached;
  emacs_value   symbol;    /* global-ref'd symbol, for accessor */
} synonym_impl_t;

/* Re-resolve the cached target from the symbol's current value. */
static void syn_resolve(cl_stream_t *s) {
  synonym_impl_t *si = (synonym_impl_t *)s->impl;
  if (!si || !si->symbol) return;
  emacs_env *env = cl_env();
  if (!env) return;

  emacs_value val = efuncall_cached(env, syms.fn_symbol_value, 1, &si->symbol);
  if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
    env->non_local_exit_clear(env);   /* unbound: keep the cached target */
    return;
  }
  if (!env->eq(env, env->type_of(env, val), eintern(env, "user-ptr")))
    return;                           /* not a stream: keep the cached target */
  cl_stream_t *t = euser_ptr_stream_get(env, val);
  if (t) si->cached = t;
}

static cl_stream_t *syn_target(cl_stream_t *s) {
  return ((synonym_impl_t *)s->impl)->cached;
}

static ptrdiff_t syn_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  cl_stream_t *t = syn_target(s);
  if (!t || !stream_is_open(t)) return 0;
  return stream_read(t, buf, n);
}

static ptrdiff_t syn_write(cl_stream_t *s, const void *buf, ptrdiff_t n) {
  cl_stream_t *t = syn_target(s);
  if (!t || !stream_is_open(t)) return 0;
  return stream_write(t, buf, n);
}

static bool syn_listen(cl_stream_t *s) {
  cl_stream_t *t = syn_target(s);
  if (!t) return false;
  return stream_listen(t);
}

static bool syn_direct(cl_stream_t *s, const void **addr, size_t *len) {
  cl_stream_t *t = syn_target(s);
  if (!t || !stream_is_open(t)) return false;
  return stream_direct(t, addr, len);
}

static void syn_advance(cl_stream_t *s, ptrdiff_t n) {
  cl_stream_t *t = syn_target(s);
  if (t && stream_is_open(t)) stream_advance(t, n);
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
  cl_stream_t *t = syn_target(s);
  if (!t) return -1;
  return stream_file_position(t);
}

static bool syn_set_file_position(cl_stream_t *s, ptrdiff_t pos) {
  cl_stream_t *t = syn_target(s);
  if (!t) return false;
  return stream_set_file_position(t, pos);
}

static ptrdiff_t syn_file_length(cl_stream_t *s) {
  cl_stream_t *t = syn_target(s);
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
  .direct        = syn_direct,
  .advance       = syn_advance,
  .file_position = syn_file_position,
  .set_file_position = syn_set_file_position,
  .file_length   = syn_file_length,
  .resolve       = syn_resolve,
};

cl_stream_t *stream_create_synonym(cl_stream_t *target, emacs_env *env,
                                    emacs_value symbol) {
  synonym_impl_t *si = calloc(1, sizeof(*si));
  if (!si) return NULL;
  si->cached = target;
  if (env && env->is_not_nil(env, symbol))
    si->symbol = env->make_global_ref(env, symbol);

  cl_stream_t *s = stream_alloc(&synonym_stream_ops, si, CL_STREAM_KIND_SYNONYM,
                                CL_STREAM_FLAG_INPUT | CL_STREAM_FLAG_OUTPUT);
  if (!s) { free(si); return NULL; }
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
  /* CL: closing a constructed stream does not close its constituents. */
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

/* Window from the first non-exhausted constituent. */
static bool concat_direct(cl_stream_t *s, const void **addr, size_t *len) {
  concat_impl_t *ci = (concat_impl_t *)s->impl;
  if (!ci) return false;
  while (ci->current < ci->count) {
    if (stream_direct(ci->streams[ci->current], addr, len)) return true;
    ci->current++;
  }
  return false;
}

static void concat_advance(cl_stream_t *s, ptrdiff_t n) {
  concat_impl_t *ci = (concat_impl_t *)s->impl;
  if (ci && ci->current < ci->count)
    stream_advance(ci->streams[ci->current], n);
}

static int concat_close(cl_stream_t *s) {
  /* CL: closing a constructed stream does not close its constituents. */
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
  .direct  = concat_direct,
  .advance = concat_advance,
};

/* ── constructors ────────────────────────────────────────────────────── */

/* Copy STREAMS into a freshly allocated array of COUNT elements. */
static cl_stream_t **copy_stream_array(cl_stream_t **streams, ptrdiff_t count) {
  cl_stream_t **copy = malloc((size_t)count * sizeof(cl_stream_t *));
  if (!copy) return NULL;
  memcpy(copy, streams, (size_t)count * sizeof(cl_stream_t *));
  return copy;
}

cl_stream_t *stream_create_broadcast(cl_stream_t **streams, ptrdiff_t count) {
  broadcast_impl_t *bc = calloc(1, sizeof(*bc));
  if (!bc) return NULL;
  bc->streams = copy_stream_array(streams, count);
  if (!bc->streams) { free(bc); return NULL; }
  bc->count = count;

  cl_stream_t *s = stream_alloc(&broadcast_stream_ops, bc,
                                CL_STREAM_KIND_BROADCAST, CL_STREAM_FLAG_OUTPUT);
  if (!s) { free(bc->streams); free(bc); return NULL; }
  return s;
}

cl_stream_t *stream_create_concatenated(cl_stream_t **streams, ptrdiff_t count) {
  concat_impl_t *ci = calloc(1, sizeof(*ci));
  if (!ci) return NULL;
  ci->streams = copy_stream_array(streams, count);
  if (!ci->streams) { free(ci); return NULL; }
  ci->count = count;
  ci->current = 0;

  cl_stream_t *s = stream_alloc(&concat_stream_ops, ci,
                                CL_STREAM_KIND_CONCATENATED, CL_STREAM_FLAG_INPUT);
  if (!s) { free(ci->streams); free(ci); return NULL; }
  return s;
}

/* ── duplex streams (two-way and echo) ───────────────────────────────────
   Both consist of an input and an output stream and differ only in that
   reads on an echo stream are also written back to the output. */

typedef struct {
  cl_stream_t *input;
  cl_stream_t *output;
  bool         echo;
} duplex_impl_t;

static ptrdiff_t dx_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  duplex_impl_t *d = (duplex_impl_t *)s->impl;
  if (!d->input || !stream_is_open(d->input)) return 0;
  ptrdiff_t r = stream_read(d->input, buf, n);
  if (d->echo && r > 0 && d->output && stream_is_open(d->output))
    stream_write(d->output, buf, r);
  return r;
}

static bool dx_direct(cl_stream_t *s, const void **addr, size_t *len) {
  cl_stream_t *in = ((duplex_impl_t *)s->impl)->input;
  if (!in || !stream_is_open(in)) return false;
  return stream_direct(in, addr, len);
}

static void dx_advance(cl_stream_t *s, ptrdiff_t n) {
  cl_stream_t *in = ((duplex_impl_t *)s->impl)->input;
  if (in && stream_is_open(in)) stream_advance(in, n);
}

/* An echo stream must copy the consumed bytes to its output. */
static void echo_advance(cl_stream_t *s, ptrdiff_t n) {
  duplex_impl_t *d = (duplex_impl_t *)s->impl;
  cl_stream_t *in = d->input;
  if (!in || !stream_is_open(in)) return;
  if (d->echo && n > 0 && d->output && stream_is_open(d->output)) {
    const void *addr; size_t len;
    if (stream_direct(in, &addr, &len) && (size_t)n <= len)
      stream_write(d->output, addr, n);
  }
  stream_advance(in, n);
}

static ptrdiff_t dx_write(cl_stream_t *s, const void *buf, ptrdiff_t n) {
  cl_stream_t *out = ((duplex_impl_t *)s->impl)->output;
  if (!out || !stream_is_open(out)) return 0;
  return stream_write(out, buf, n);
}

static bool dx_listen(cl_stream_t *s) {
  cl_stream_t *in = ((duplex_impl_t *)s->impl)->input;
  return in && stream_listen(in);
}

static int dx_close(cl_stream_t *s) {
  /* CL: closing a constructed stream does not close its constituents. */
  atomic_store(&s->open, false);
  return 0;
}

static void dx_destroy(cl_stream_t *s) { free(s->impl); }

static int dx_flush(cl_stream_t *s) {
  cl_stream_t *out = ((duplex_impl_t *)s->impl)->output;
  return out ? stream_flush(out) : 0;
}

static int dx_finish(cl_stream_t *s) {
  cl_stream_t *out = ((duplex_impl_t *)s->impl)->output;
  return out ? stream_finish(out) : 0;
}

static void dx_clear_output(cl_stream_t *s) {
  cl_stream_t *out = ((duplex_impl_t *)s->impl)->output;
  if (out && out->ops && out->ops->clear_output)
    out->ops->clear_output(out);
}

static void dx_clear_input(cl_stream_t *s) {
  cl_stream_t *in = ((duplex_impl_t *)s->impl)->input;
  if (in && in->ops && in->ops->clear_input)
    in->ops->clear_input(in);
}

static ptrdiff_t dx_write_string(emacs_env *env, cl_stream_t *s, emacs_value str) {
  cl_stream_t *out = ((duplex_impl_t *)s->impl)->output;
  if (!out || !stream_is_open(out)) return 0;
  return stream_write_string(env, out, str);
}

static const cl_stream_ops_t twoway_stream_ops = {
  .read    = dx_read,   .write   = dx_write,
  .listen  = dx_listen, .close   = dx_close,
  .destroy = dx_destroy,
  .flush   = dx_flush,  .finish  = dx_finish,
  .direct  = dx_direct, .advance = dx_advance,
  .clear_output = dx_clear_output, .clear_input = dx_clear_input,
  .write_string  = dx_write_string,
};

static const cl_stream_ops_t echo_stream_ops = {
  .read    = dx_read,   .write   = dx_write,
  .listen  = dx_listen, .close   = dx_close,
  .destroy = dx_destroy,
  .flush   = dx_flush,
  .direct  = dx_direct, .advance = echo_advance,
  .clear_output = dx_clear_output, .clear_input = dx_clear_input,
  .write_string  = dx_write_string,
};

static cl_stream_t *duplex_create(cl_stream_t *input, cl_stream_t *output,
                                  bool echo) {
  duplex_impl_t *d = calloc(1, sizeof(*d));
  if (!d) return NULL;
  d->input = input;
  d->output = output;
  d->echo = echo;

  cl_stream_t *s = stream_alloc(echo ? &echo_stream_ops : &twoway_stream_ops, d,
                                echo ? CL_STREAM_KIND_ECHO : CL_STREAM_KIND_TWO_WAY,
                                CL_STREAM_FLAG_INPUT | CL_STREAM_FLAG_OUTPUT);
  if (!s) { free(d); return NULL; }
  return s;
}

cl_stream_t *stream_create_twoway(cl_stream_t *input, cl_stream_t *output) {
  return duplex_create(input, output, false);
}

cl_stream_t *stream_create_echo(cl_stream_t *input, cl_stream_t *output) {
  return duplex_create(input, output, true);
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
  emacs_value target_val = efuncall_cached(env, syms.fn_symbol_value, 1, &args[0]);
  cl_stream_t *target = euser_ptr_stream_get(env, target_val);
  if (!target) {
    signal_error(env, "Synonym target is not a stream");
    return syms.v_nil;
  }
  return stream_or_error(env, stream_create_synonym(target, env, args[0]),
                         "Cannot create synonym stream");
}

emacs_value Fsynonym_stream_symbol(emacs_env *env, ptrdiff_t nargs,
                                    emacs_value *args, void *data) {
  (void)nargs; (void)data;
  cl_stream_t *s = require_stream_of(env, args[0], &synonym_stream_ops,
                                     "Not a synonym stream");
  if (!s) return syms.v_nil;
  synonym_impl_t *si = (synonym_impl_t *)s->impl;
  return si->symbol ? si->symbol : syms.v_nil;
}

emacs_value Fmake_broadcast_stream(emacs_env *env, ptrdiff_t nargs,
                                    emacs_value *args, void *data) {
  (void)data;
  cl_stream_t **list = collect_streams(env, nargs, args);
  if (!list) return syms.v_nil;
  cl_stream_t *s = stream_create_broadcast(list, nargs);
  free(list);
  return stream_or_error(env, s, "Cannot create broadcast stream");
}

emacs_value Fmake_concatenated_stream(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value *args, void *data) {
  (void)data;
  cl_stream_t **list = collect_streams(env, nargs, args);
  if (!list) return syms.v_nil;
  cl_stream_t *s = stream_create_concatenated(list, nargs);
  free(list);
  return stream_or_error(env, s, "Cannot create concatenated stream");
}

emacs_value Fbroadcast_stream_streams(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = require_stream_of(env, args[0], &broadcast_stream_ops,
                                     "Not a broadcast stream");
  if (!s) return syms.v_nil;
  broadcast_impl_t *bc = (broadcast_impl_t *)s->impl;
  return streams_to_list(env, bc->streams, bc->count);
}

emacs_value Fconcatenated_stream_streams(emacs_env *env, ptrdiff_t nargs,
                                          emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = require_stream_of(env, args[0], &concat_stream_ops,
                                     "Not a concatenated stream");
  if (!s) return syms.v_nil;
  concat_impl_t *ci = (concat_impl_t *)s->impl;
  /* CL: only the streams still to be read, starting with the current one. */
  return streams_to_list(env, ci->streams + ci->current,
                         ci->count - ci->current);
}

emacs_value Fmake_two_way_stream(emacs_env *env, ptrdiff_t nargs,
                                  emacs_value *args, void *data) {
  (void)data;
  if (nargs < 2) {
    signal_error(env, "cl-make-two-way-stream requires 2 arguments");
    return syms.v_nil;
  }
  cl_stream_t *in  = require_stream(env, args[0], "Invalid stream");
  if (!in) return syms.v_nil;
  cl_stream_t *out = require_stream(env, args[1], "Invalid stream");
  if (!out) return syms.v_nil;
  return stream_or_error(env, stream_create_twoway(in, out),
                         "Cannot create two-way stream");
}

emacs_value Ftwo_way_stream_input_stream(emacs_env *env, ptrdiff_t nargs,
                                          emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = require_stream_of(env, args[0], &twoway_stream_ops,
                                     "Not a two-way stream");
  if (!s) return syms.v_nil;
  return euser_ptr_stream_ref(env, ((duplex_impl_t *)s->impl)->input);
}

emacs_value Ftwo_way_stream_output_stream(emacs_env *env, ptrdiff_t nargs,
                                           emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = require_stream_of(env, args[0], &twoway_stream_ops,
                                     "Not a two-way stream");
  if (!s) return syms.v_nil;
  return euser_ptr_stream_ref(env, ((duplex_impl_t *)s->impl)->output);
}

emacs_value Fmake_echo_stream(emacs_env *env, ptrdiff_t nargs,
                               emacs_value *args, void *data) {
  (void)data;
  if (nargs < 2) {
    signal_error(env, "cl-make-echo-stream requires 2 arguments");
    return syms.v_nil;
  }
  cl_stream_t *in  = require_stream(env, args[0], "Invalid stream");
  if (!in) return syms.v_nil;
  cl_stream_t *out = require_stream(env, args[1], "Invalid stream");
  if (!out) return syms.v_nil;
  return stream_or_error(env, stream_create_echo(in, out),
                         "Cannot create echo stream");
}

emacs_value Fecho_stream_input_stream(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = require_stream_of(env, args[0], &echo_stream_ops,
                                     "Not an echo stream");
  if (!s) return syms.v_nil;
  return euser_ptr_stream_ref(env, ((duplex_impl_t *)s->impl)->input);
}

emacs_value Fecho_stream_output_stream(emacs_env *env, ptrdiff_t nargs,
                                        emacs_value *args, void *data) {
  (void)data; (void)nargs;
  cl_stream_t *s = require_stream_of(env, args[0], &echo_stream_ops,
                                     "Not an echo stream");
  if (!s) return syms.v_nil;
  return euser_ptr_stream_ref(env, ((duplex_impl_t *)s->impl)->output);
}
