#include "stream.h"
#include <stdlib.h>
#include <string.h>

/* ── dispatch helpers ────────────────────────────────────────────────── */

#define LOCK(s)   pthread_mutex_lock(&(s)->mutex)
#define UNLOCK(s) pthread_mutex_unlock(&(s)->mutex)
#define RESOLVE(s) do { if ((s)->ops && (s)->ops->resolve) (s)->ops->resolve(s); } while (0)

ptrdiff_t stream_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0 || !s->ops || !s->ops->read) return 0;
  if (!atomic_load(&s->open)) return 0;
  RESOLVE(s);
  LOCK(s);

  /* drain pushback ring buffer first */
  if (s->pushback_head != s->pushback_tail) {
    ptrdiff_t count = 0;
    unsigned char *dest = (unsigned char *)buf;
    while (s->pushback_head != s->pushback_tail && count < n) {
      dest[count++] = (unsigned char)s->pushback_chars[s->pushback_head];
      s->pushback_head = (s->pushback_head + 1) & 255;
    }
    UNLOCK(s);
    return count;
  }

  ptrdiff_t r = s->ops->read(s, buf, n);
  UNLOCK(s);
  return r;
}

ptrdiff_t stream_write(cl_stream_t *s, const void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0 || !s->ops || !s->ops->write) return 0;
  if (!atomic_load(&s->open)) return 0;
  RESOLVE(s);
  LOCK(s);
  ptrdiff_t r = s->ops->write(s, buf, n);

  /* column tracking for cl-fresh-line */
  if (r > 0) {
    const unsigned char *p = (const unsigned char *)buf;
    ptrdiff_t after_nl = 0;
    bool found_nl = false;
    for (ptrdiff_t i = r - 1; i >= 0; i--) {
      if (p[i] == '\n') { after_nl = r - 1 - i; found_nl = true; break; }
    }
    s->column = found_nl ? after_nl : s->column + r;
    s->last_char = (int)p[r - 1];
  }

  UNLOCK(s);
  return r;
}

int stream_close(cl_stream_t *s) {
  if (!s) return -1;
  if (!atomic_load(&s->open)) return 0;
  RESOLVE(s);
  LOCK(s);
  int r = 0;
  if (s->ops && s->ops->close)
    r = s->ops->close(s);
  else
    atomic_store(&s->open, false);
  UNLOCK(s);
  return r;
}

int stream_flush(cl_stream_t *s) {
  if (!s || !s->ops || !s->ops->flush) return 0;
  if (!atomic_load(&s->open)) return 0;
  RESOLVE(s);
  LOCK(s);
  int r = s->ops->flush(s);
  UNLOCK(s);
  return r;
}

int stream_finish(cl_stream_t *s) {
  if (!s || !s->ops || !s->ops->finish) return 0;
  if (!atomic_load(&s->open)) return 0;
  RESOLVE(s);
  LOCK(s);
  int r = s->ops->finish(s);
  UNLOCK(s);
  return r;
}

bool stream_listen(cl_stream_t *s) {
  if (!s || !s->ops || !s->ops->listen) return false;
  if (!atomic_load(&s->open)) return false;
  RESOLVE(s);
  LOCK(s);
  if (s->pushback_head != s->pushback_tail) { UNLOCK(s); return true; }
  bool r = s->ops->listen(s);
  UNLOCK(s);
  return r;
}

void stream_unread(cl_stream_t *s, int ch) {
  if (!s) return;
  LOCK(s);
  int next = (s->pushback_tail + 1) & 255;
  if (next == s->pushback_head) { UNLOCK(s); return; }
  s->pushback_chars[s->pushback_tail] = ch;
  s->pushback_tail = next;
  UNLOCK(s);
}

bool stream_direct(cl_stream_t *s, const void **addr, size_t *len) {
  if (!s || !s->ops || !s->ops->direct || !addr || !len) return false;
  if (!atomic_load(&s->open)) return false;
  RESOLVE(s);
  LOCK(s);
  bool ok = s->ops->direct(s, addr, len);
  UNLOCK(s);
  return ok;
}

void stream_advance(cl_stream_t *s, ptrdiff_t n) {
  if (!s || n <= 0 || !s->ops || !s->ops->advance) return;
  RESOLVE(s);
  LOCK(s);
  s->ops->advance(s, n);
  UNLOCK(s);
}

ptrdiff_t stream_write_string(emacs_env *env, cl_stream_t *s, emacs_value str) {
  if (!s || !env) return 0;
  if (!atomic_load(&s->open)) return 0;

  if (s->ops && s->ops->write_string) {
    RESOLVE(s);
    LOCK(s);
    ptrdiff_t r = s->ops->write_string(env, s, str);
    UNLOCK(s);
    return r;
  }

  ptrdiff_t slen = 0;
  if (!env->copy_string_contents(env, str, NULL, &slen)) return 0;
  if (slen == 0) return 0;

  unsigned char *buf = malloc((size_t)slen);
  if (!buf) return -1;

  ptrdiff_t len = slen;
  if (!env->copy_string_contents(env, str, (char *)buf, &len)) {
    free(buf);
    return -1;
  }
  len--;

  ptrdiff_t r = stream_write(s, buf, len);
  free(buf);
  return r;
}

ptrdiff_t stream_file_position(cl_stream_t *s) {
  if (!s || !s->ops || !s->ops->file_position) return -1;
  if (!atomic_load(&s->open)) return -1;
  RESOLVE(s);
  LOCK(s);
  ptrdiff_t r = s->ops->file_position(s);
  UNLOCK(s);
  return r;
}

bool stream_set_file_position(cl_stream_t *s, ptrdiff_t pos) {
  if (!s || pos < 0 || !s->ops || !s->ops->set_file_position) return false;
  if (!atomic_load(&s->open)) return false;
  RESOLVE(s);
  LOCK(s);
  bool r = s->ops->set_file_position(s, pos);
  UNLOCK(s);
  return r;
}

ptrdiff_t stream_file_length(cl_stream_t *s) {
  if (!s || !s->ops || !s->ops->file_length) return -1;
  RESOLVE(s);
  LOCK(s);
  ptrdiff_t r = s->ops->file_length(s);
  UNLOCK(s);
  return r;
}
