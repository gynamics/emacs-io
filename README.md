# emacs-io

CL-style stream I/O as a dynamic module for GNU Emacs 31.

```elisp
(module-load "emacs-io.so")
```

## Why

Emacs buffers are expensive for sequential I/O — they trigger hooks, maintain
undo history, manage markers, and force gap-buffer moves on every insert.
This module provides a lightweight stream abstraction that sidesteps all of
that, backed by raw POSIX I/O and in-memory C buffers.

## Quick start

```elisp
;; build
$ make

;; run tests
$ make test      # 48 unit tests
$ make bench     # 6 performance benchmarks
```

## Stream types

| Type | Backend | Use |
|------|---------|-----|
| `cl-open` | POSIX `open(2)` + userspace buffering | File I/O |
| `cl-make-string-input-stream` | C `char*` buffer with cursor | Parse in-memory data |
| `cl-make-string-output-stream` | Growable C `char*` buffer | Build strings efficiently |
| `cl-make-buffer-input-stream` | Emacs buffer via `buffer-substring-no-properties` | Bridge to existing buffers |

Standard streams (`*standard-input*`, `*standard-output*`, `*error-output*`)
are initialized automatically, mapped to file descriptors 0, 1, and 2.

## API (35 functions)

### Stream lifecycle
`cl-open` `cl-close` `cl-open-stream-p`
`cl-input-stream-p` `cl-output-stream-p` `cl-stream-element-type`

### String streams
`cl-make-string-input-stream` `cl-make-string-output-stream`
`cl-get-output-stream-string`

### Character I/O
`cl-read-char` `cl-read-line` `cl-read-byte`
`cl-peek-char` `cl-unread-char` `cl-listen`
`cl-write-char` `cl-write-string` `cl-write-line` `cl-write-byte`
`cl-terpri` `cl-fresh-line`

### Stream control
`cl-finish-output` (fsync) `cl-force-output` (fflush)
`cl-clear-output` `cl-clear-input`

### Sequence I/O
`cl-read-sequence` `cl-write-sequence`

### Lisp reader
`cl-read` `cl-read-from-string` `cl-read-preserving-whitespace`

### Lisp printer
`cl-write` `cl-prin1` `cl-princ` `cl-print`
`cl-prin1-to-string` `cl-princ-to-string`

## Performance

Representative benchmarks (Emacs 31.0.90, Linux x86-64):

```
read-sequence 100K        stream=  2.3ms  buffer= 35.9ms  speedup=15.7x
write-sequence 100K       stream=  2.3ms  buffer= 36.7ms  speedup=15.8x
string-output 100K        stream= 18.3ms  buffer= 35.7ms  speedup= 1.9x
```

## Architecture

Streams use a vtable dispatch (`cl_stream_ops`) with a mutex per stream.
Concrete implementations live in dedicated translation units:

- `fdstream.c` — POSIX fd I/O with 8KB userspace ring buffers
- `strstream.c` — string I/O and Emacs buffer chunk reader
- `stream.c` — dispatch layer (lock, drain pushback, call vtable)
- `reader.c` / `printer.c` — CL I/O primitives
- `lispio.c` — `cl-read` (state machine → Emacs `read-from-string`) and
  `cl-prin1`/`cl-princ` (→ Emacs `prin1-to-string`)
- `fileio.c` — `cl-open` with CL keyword args, stream predicates

## Known issues

- **ERT byte-compiler quirk** (Emacs 31.0.90): `cl-fresh-line` and `cl-prin1`
  trigger `(invalid-function #<user-ptr>)` when called with a stream argument
  inside an `ert-deftest` body.  Functions work correctly in normal evaluation.
  Tests use `*-to-string` variants to work around this.
- **Line reading overhead**: `cl-read-line` uses per-character reads through
  the dispatch layer.  Buffer-based `buffer-substring` is faster for
  existing complete text.  Bulk `cl-read-sequence` wins on throughput
  for pre-sized data.

## License

GPL-3.0+
