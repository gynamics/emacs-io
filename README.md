# emacs-io — CL-Style Stream I/O for GNU Emacs

**A native dynamic module for Emacs 31 that delivers high-performance,
thread-safe stream I/O — bypassing buffer gap-machinery entirely.**

```elisp
(add-to-list 'load-path "/path/to/emacs-io")
(require 'emacs-io)
```

## Why

Emacs buffers are the universal I/O medium — and the bottleneck.  Every
`insert` triggers modification hooks, maintains undo history, repositions
markers and overlays, and forces gap-buffer moves.  A 100 MB log file loaded
into a temp buffer allocates 100 MB of gap-buffer, then garbage-collects it.

Streams bypass all of that.  Raw `read(2)` / `write(2)` through userspace
ring buffers, zero copy within string streams, and no hook machinery.

## Quick start

```bash
make                  # → emacs-io-core.so
make test             # 223 tests: unit + Common Lisp conformance
make bench            # 12 performance benchmarks
```

`emacs-io.el` is the entry point: it `require`s the native core module
(`emacs-io-core.so`) — which provides the byte/character/sequence primitives,
the stream constructors, the predicates and `cl-read` — and then defines the
reader/printer glue (`cl-write`, `cl-prin1`, `cl-pprint`, `cl-read-from-string`,
`cl-terpri`, `cl-y-or-n-p`, ...) plus the `cl-with-open-file` /
`cl-with-open-stream` macros.

```elisp
(add-to-list 'load-path "/path/to/emacs-io")
(require 'emacs-io)            ;; loads emacs-io-core.so and the Lisp layer
```

The native module deliberately has a different base name from the entry
(`emacs-io-core.so` vs `emacs-io.el`): `load-suffixes` tries `.so` before
`.el`, so sharing a base name would make `(require 'emacs-io)` load the module
instead of the entry point.

`EMACS_IO_BENCH_FULL=1 make bench` enables the 10 MB-scale tests.

`emacs-module.h` is not in the repo — it comes from Emacs 31 dev headers.

## Stream types

| Constructor | Backend | Direction |
|---|---|---|
| `cl-open` | POSIX `open(2)`, 8 KB ring buffers, `O_NONBLOCK` support | any |
| `cl-make-string-input-stream` | C `const char*` with position cursor | input |
| `cl-make-string-output-stream` | Block-chain growable accumulator | output |
| `cl-make-buffer-input-stream` | Emacs buffer snapshot at creation time | input |
| `cl-make-buffer-output-stream` | Writes insert into an Emacs buffer | output |
| `cl-make-buffer-io-stream` | Snapshot reads + buffer inserts, shared cursor | input / output |

Standard streams (`*standard-input*`, `*standard-output*`, `*error-output*`,
`*trace-output*`, `*debug-io*`, `*query-io*`, `*terminal-io*`) are
initialized at module load, mapped to file descriptors 0, 1, and 2.

## API

### Stream lifecycle

| Function | Signature | Description |
|---|---|---|
| `cl-open` | `(cl-open filename &key direction if-exists element-type)` | Open a file, return a stream (`:if-exists` defaults to `:error`) |
| `cl-close` | `(cl-close stream)` | Close the stream |
| `cl-open-stream-p` | `(cl-open-stream-p object)` | Return t if object is an open stream |
| `cl-input-stream-p` | `(cl-input-stream-p stream)` | Return t if stream supports input |
| `cl-output-stream-p` | `(cl-output-stream-p stream)` | Return t if stream supports output |
| `cl-stream-element-type` | `(cl-stream-element-type stream)` | Return the type of elements read/written |
| `cl-file-position` | `(cl-file-position stream &optional position)` | Return or set the current position (`:start`/`:end` accepted) |
| `cl-ftruncate` | `(cl-ftruncate stream length)` | Truncate file stream to given length |
| `cl-file-regular-p` | `(cl-file-regular-p stream)` | Return t if stream is a regular file |
| `cl-file-tty-p` | `(cl-file-tty-p stream)` | Return t if stream is a terminal |

### String streams

| Function | Signature | Description |
|---|---|---|
| `cl-make-string-input-stream` | `(cl-make-string-input-stream string &optional start end)` | Create an input stream from a string |
| `cl-make-string-output-stream` | `(cl-make-string-output-stream &key element-type size)` | Create an output stream that accumulates in a string |
| `cl-get-output-stream-string` | `(cl-get-output-stream-string stream)` | Return the accumulated output as a string (clears the stream) |
| `cl-make-buffer-input-stream` | `(cl-make-buffer-input-stream buffer &key start end)` | Create an input stream from a buffer's text |
| `cl-make-buffer-output-stream` | `(cl-make-buffer-output-stream buffer)` | Create an output stream that inserts into a buffer |
| `cl-make-buffer-io-stream` | `(cl-make-buffer-io-stream buffer &key start end)` | Bidirectional buffer stream (reads snapshot, writes to buffer) |

### Character I/O

| Function | Signature | Description |
|---|---|---|
| `cl-read-char` | `(cl-read-char &optional stream eof-error-p eof-value)` | Read and return one character |
| `cl-read-line` | `(cl-read-line &optional stream eof-error-p eof-value)` | Read a line, returns `(line . missing-newline-p)` |
| `cl-read-byte` | `(cl-read-byte stream &optional eof-error-p eof-value)` | Read and return one byte |
| `cl-peek-char` | `(cl-peek-char &optional peek-type stream eof-error-p eof-value recursive-p)` | Peek next char (`t` skips whitespace; a char skips up to and leaves it) |
| `cl-unread-char` | `(cl-unread-char character &optional stream)` | Push a character back onto the stream |
| `cl-listen` | `(cl-listen &optional stream)` | Return t if a character is available to read |
| `cl-write-char` | `(cl-write-char character &optional stream)` | Write a character to the stream |
| `cl-write-string` | `(cl-write-string string &optional stream &key start end)` | Write a string to the stream |
| `cl-write-line` | `(cl-write-line string &optional stream &key start end)` | Write a string followed by a newline |
| `cl-write-byte` | `(cl-write-byte byte stream)` | Write a byte to the stream |
| `cl-terpri` | `(cl-terpri &optional stream)` | Output a newline |
| `cl-fresh-line` | `(cl-fresh-line &optional stream)` | Output a newline if not at the start of a line |

### Bulk I/O

| Function | Signature | Description |
|---|---|---|
| `cl-read-sequence` | `(cl-read-sequence sequence stream &key start end)` | Read elements into a vector, returns index of first unfilled element |
| `cl-write-sequence` | `(cl-write-sequence sequence stream &key start end)` | Write elements of a vector to the stream |
| `cl-read-string` | `(cl-read-string n stream &optional eof-error-p eof-value)` | Read up to N bytes, return a unibyte string |
| `cl-read-chunk` | `(cl-read-chunk stream &optional max-bytes)` | Read up to MAX-BYTES, return `(chunk . eof-p)` |

### Stream control

| Function | Signature | Description |
|---|---|---|
| `cl-finish-output` | `(cl-finish-output &optional stream)` | fsync(2) the stream — ensure data reaches disk |
| `cl-force-output` | `(cl-force-output &optional stream)` | fflush(3) the stream — empty internal buffers |
| `cl-clear-output` | `(cl-clear-output &optional stream)` | Discard pending output |
| `cl-clear-input` | `(cl-clear-input &optional stream)` | Discard pending input |

### Lisp reader / printer

| Function | Signature | Description |
|---|---|---|
| `cl-read` | `(cl-read &optional stream eof-error-p eof-value recursive-p)` | Read one Lisp object from the stream |
| `cl-read-from-string` | `(cl-read-from-string string &optional eof-error-p eof-value &key start end preserve-whitespace)` | Read one Lisp object from a string |
| `cl-write` | `(cl-write object &key stream ...)` | Write an object (`:stream` defaults to `*standard-output*`; `:escape nil` prints princ-style) |
| `cl-prin1` | `(cl-prin1 object &optional stream)` | Write an object in a form readable by `cl-read` |
| `cl-princ` | `(cl-princ object &optional stream)` | Write an object in human-readable form |
| `cl-print` | `(cl-print object &optional stream)` | Write a newline, then the object, then a space |
| `cl-pprint` | `(cl-pprint object &optional stream)` | Pretty-print an object (returns nil) |
| `cl-prin1-to-string` | `(cl-prin1-to-string object)` | Return a string of the object, readable by `cl-read` |
| `cl-princ-to-string` | `(cl-princ-to-string object)` | Return a human-readable string of the object |
| `cl-write-to-string` | `(cl-write-to-string object &key ...)` | Return the printed representation as a string |

## Practical uses

### Log processing without temp buffers

```elisp
(cl-with-open-file (s "/var/log/app.log" :direction :input)
  (cl-with-open-file (out "/tmp/errors.log" :direction :output :if-exists :supersede)
    (while (cl-listen s)
      (let ((line (cl-read-line s nil nil)))
        (when (and (car line) (string-match "ERROR" (car line)))
          (cl-write-line (car line) out))))))
```

### Fast sequence encoding

```elisp
(let* ((v (make-vector 100000 ?x))
       (s (cl-make-string-output-stream :size (* 100000 8))))
  (cl-write-sequence v s)
  (cl-get-output-stream-string s))
```

### File read/write

```elisp
(let ((s (cl-open "/tmp/data.bin" :direction :output :if-exists :supersede)))
  (cl-write-string (make-string (* 10 1024 1024) ?x) s)
  (cl-close s))

(let ((s (cl-open "/tmp/data.bin" :direction :input)))
  (cl-read-string (* 10 1024 1024) s)
  (cl-close s))
```

### Chunked streaming

```elisp
(let ((s (cl-open "/tmp/large.json" :direction :input)))
  (while t
    (let ((pair (cl-read-chunk s 65536)))
      (process-chunk (car pair))
      (when (cdr pair) (cl-close s) (cl-return)))))
```

### Buffer I/O — read and write to an Emacs buffer

```elisp
(let ((buf (generate-new-buffer "*work*")))
  (with-current-buffer buf (insert "HEADER\n"))
  (let ((s (cl-make-buffer-io-stream buf)))
    (cl-write-line "data1" s)      ;; inserts into buffer
    (cl-write-line "data2" s)
    (cl-close s))
  (with-current-buffer buf
    (buffer-string)))
;; → "HEADER\ndata1\ndata2\n"
```

### Broadcast to multiple destinations

```elisp
(let ((log (cl-open "/tmp/out.log" :direction :output :if-exists :supersede))
      (sos (cl-make-string-output-stream)))
  (let ((bc (cl-make-broadcast-stream log sos)))
    (cl-write-string "goes to both" bc)
    (cl-close bc))
  (cl-close log)
  (cl-get-output-stream-string sos))
;; → "goes to both"
```

### Two-way stream — separate read/write channels

```elisp
(let ((in  (cl-make-string-input-stream "request"))
      (out (cl-make-string-output-stream)))
  (let ((tw (cl-make-two-way-stream in out)))
    (let ((req (cl-read-line tw)))
      (cl-write-string (concat "echo: " (car req)) tw))
    (cl-close tw))
  (cl-get-output-stream-string out))
;; → "echo: request"
```

### File truncation

```elisp
(let ((s (cl-open "/tmp/data.bin" :direction :io :if-exists :overwrite)))
  (cl-write-string "abcdefgh" s)
  (cl-ftruncate s 3)               ;; truncate to 3 bytes
  (cl-file-position s 0)           ;; seek back to start
  (cl-read-string 3 s))
;; → "abc"
```

### Binary I/O

```elisp
(let ((in  (cl-open "/tmp/binary.dat" :direction :input))
      (out (cl-open "/tmp/copy.dat" :direction :output :if-exists :supersede)))
  (while t
    (let ((byte (cl-read-byte in nil :eof)))
      (if (eq byte :eof) (cl-return))
      (cl-write-byte byte out)))
  (cl-close in) (cl-close out))
```

## Performance

All numbers from Emacs 31.0.90, Linux x86-64, GCC 14 `-O2`.  Every test
compares stream I/O against the most efficient Emacs native equivalent.

| Test | Stream | Buffer | Speedup |
|---|---|---|---|
| `cl-read-sequence` 100 K elements | 2.2 ms | 34.6 ms | **15.9×** |
| `cl-write-sequence` 100 K elements | 2.5 ms | 31.8 ms | **12.9×** |
| Log grep 10 K lines | 1.7 ms | 4.8 ms | **2.9×** |
| Comint incremental 10 K lines | 1.6 ms | 4.7 ms | **2.9×** |
| Per-char write 100 K | 19.5 ms | 35.5 ms | **1.8×** |
| Line read 10 K lines | 2.0 ms | 3.3 ms | **1.6×** |

Sequence I/O wins by **16×** by eliminating per-element C↔Lisp crossings:
a single C `memcpy` of 100 KB replaces 100,000 iterations of `(aset vec i
(char-after ...))`.  File I/O wins by **10×** via raw `write(2)` bypassing
`write-region` encoding and mode hooks.  Line processing uses a 4 KB
bulk-chunk scanner with C-level newline detection instead of per-byte
dispatch.

## Architecture

```
src/stream.h     — cl_stream struct, vtable, impl structs, mem_cursor, public API
src/stream.c     — dispatch layer, in-memory cursor ops, live-stream registry, allocation
src/fdstream.c   — POSIX fd I/O with 8 KB userspace ring buffers
src/mmapstream.c — mmap-backed input for regular files (zero-copy reads)
src/strstream.c  — string-input (mem_cursor), string-output (block-chain)
src/bufstream.c  — buffer input (mem_cursor), buffer output (insert), buffer I/O (bidirectional)
src/reader.c     — cl-read (incremental read-from-string), cl-read-char, cl-read-line
                   (CRLF-aware), bulk chunk reads, read-delimited-list
src/printer.c    — cl-write-char/string/line/byte, fresh-line, flush/force/finish/clear
src/fileio.c     — cl-open, cl-close, predicates, file-position, file-length, ftruncate
src/stdstream.c  — standard stream variable init
src/compstream.c — synonym, broadcast, concatenated, two-way, echo streams
src/util.c/h     — emacs_env wrappers, defun trampoline, seq_classify, stream_or_error, eof_result
src/syms.h       — pre-interned symbol table
src/strbuf.h     — growable byte buffer
src/subrs.h      — declarations of every registered subr
src/emacs-io.c   — emacs_module_init + subr registration table, provide("emacs-io-core")
emacs-io.el      — entry point: requires emacs-io-core, then defines the Lisp
                   layer (cl-write/-to-string, cl-prin1/princ/print/pprint,
                   cl-prin1/princ-to-string, cl-terpri, cl-read-from-string,
                   cl-y-or-n-p, cl-yes-or-no-p, cl-file-string-length) and the
                   cl-with-open-file / cl-with-open-stream macros
```

Every I/O operation acquires a per-stream `pthread_mutex_t`.  Streams are
flagged `O_NONBLOCK` and queried with `cl-listen` — ready for `select`/`poll`
event loops.  `cl-read-chunk` and `cl-read-string` use `stream_direct` for
zero-copy reads from mmap and string-input streams.

## Notes

- **C core + Lisp entry point**: `emacs-io-core.so` provides the
  byte/character/sequence primitives, the stream constructors, the
  predicates, and `cl-read` / `cl-read-delimited-list` (which need direct
  access to the stream pushback ring).  Functions that are just glue over
  Emacs' own reader and printer (`cl-write`, `cl-write-to-string`,
  `cl-prin1`, `cl-princ`, `cl-print`, `cl-pprint`, `cl-prin1-to-string`,
  `cl-princ-to-string`, `cl-terpri`, `cl-read-from-string`, `cl-y-or-n-p`,
  `cl-yes-or-no-p`, `cl-file-string-length`) live in `emacs-io.el`, which
  requires the core module — so the install directory must be on `load-path`
  and `emacs-io.el` must sit next to `emacs-io-core.so`.

- **Windows unsupported**: Linux/BSD/macOS.  Uses `memfd_create`, `O_NONBLOCK`, `fsync`, `mmap`, `pthread`, `ftruncate`.

- **Buffer output streams**: byte-level writes (write-char, write-byte) are buffered in a strbuf and flushed to the target buffer on close or on the next string-write.  No data is lost.

- **Synonym streams**: re-resolve the symbol's value before each operation (CL-compatible), via the stream `resolve` hook, outside the stream mutex.

- **End-of-file condition**: EOF signals `(signal 'end-of-file ...)` — use `(condition-case ... (end-of-file ...))` to handle, not generic `error`.  `cl-read-line` returns a dotted pair `(line . missing-newline-p)` (and `(eof-value . t)` on immediate EOF), since Elisp has no multiple values.

- **Common Lisp conformance**: behaviour follows the ANSI spec; `make test` runs the unit tests plus `test/emacs-io-conformance-test.el`, which exercises the CL boundary cases (`cl-open` `:if-exists` defaults to `:error`; `cl-close` of a constructed stream leaves its constituents open; `cl-get-output-stream-string` clears; `cl-write-char` writes UTF-8; `cl-file-position` accepts `:start`/`:end`; and so on).

- **ERT byte-compiler quirk** (Emacs 31.0.90): `cl-fresh-line` and `cl-prin1` misbehave when called — even indirectly — from inside an `ert-deftest` body (for `cl-prin1`, because Emacs' own `cl-print` library also defines an autoloaded `cl-prin1` that wins in byte-compiled code).  The functions work correctly in normal code; the conformance tests probe them at load time, outside any test lambda.

## License

GPL-3.0+
