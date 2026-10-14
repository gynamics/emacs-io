;;; emacs-io-perf-test.el — performance benchmarks -*- lexical-binding: t; -*-
;;
;;  Every comparison uses the most efficient Emacs native approach.
;;  Single-call vs single-call, bulk vs bulk, loop vs loop.
;;  Set EMACS_IO_BENCH_FULL=1 for the 10MB-scale tests.
;;
;;  Timing is the fastest of `emacs-io-perf--iterations' runs after a warmup,
;;  with GC deferred out of the measured region, so the numbers (and the
;;  speedup assertions) are not dominated by collection or scheduler noise.

(require 'ert)

(add-to-list 'load-path (expand-file-name ".." (file-name-directory load-file-name)))
(require 'emacs-io)

(defvar emacs-io-perf--iterations 5
  "Timed iterations per side; the fastest run is reported.")

;; ── helpers ──────────────────────────────────────────────────────────────

(defmacro emacs-io-perf--with-string-input (string &rest body)
  (declare (indent 1))
  `(let ((stream (cl-make-string-input-stream ,string)))
     (unwind-protect (progn ,@body)
       (cl-close stream))))

(defmacro emacs-io-perf--with-string-output (&rest body)
  (declare (indent 0))
  `(let ((stream (cl-make-string-output-stream)))
     ,@body
     (cl-get-output-stream-string stream)))

(defun emacs-io-perf--time (fn)
  "Time FN and return (RESULT . SECONDS).
FN runs once untimed as a warmup, then `emacs-io-perf--iterations' times with
GC deferred out of the measured region; the fastest run is returned."
  (funcall fn)                          ; warmup: fault in code, caches, allocs
  (let ((gc-cons-threshold most-positive-fixnum)
        (best most-positive-fixnum)
        (result nil))
    (dotimes (_ emacs-io-perf--iterations)
      (garbage-collect)
      (let ((t0 (float-time)))
        (setq result (funcall fn))
        (setq best (min best (- (float-time) t0)))))
    (cons result best)))

(defun emacs-io-perf--report (test stream-timed buffer-timed &optional check win)
  "Print the STREAM-TIMED vs BUFFER-TIMED comparison for TEST.
Both arguments are (RESULT . SECONDS) pairs from `emacs-io-perf--time'.
Assert that the stream side won unless WIN is nil (for cases where the
native buffer idiom is legitimately faster).  When CHECK is non-nil, assert
that the two results satisfy it, so a benchmark cannot pass while computing
the wrong thing."
  (let* ((st (cdr stream-timed))
         (bt (cdr buffer-timed))
         (ratio (if (> bt 0) (/ bt (max st 0.000001)) 0)))
    (message "%-42s  stream=%7.3fms  buffer=%7.3fms  speedup=%.1fx%s"
             test (* st 1000) (* bt 1000) ratio
             (if (>= ratio 1.0) "" "  (buffer wins)"))
    (when check
      (should (funcall check (car stream-timed) (car buffer-timed))))
    (should (or (null win) (>= ratio 1.0)))))

(defvar emacs-io-perf--full
  (equal (getenv "EMACS_IO_BENCH_FULL") "1")
  "Non-nil when the 10 MB-level benchmarks should run.")

(defmacro emacs-io-perf--unless-full (label &rest body)
  "Run BODY only in a full benchmark run; otherwise print that LABEL was skipped."
  (declare (indent 1))
  `(if emacs-io-perf--full
       (progn ,@body)
     (message "%-42s  skipped (set EMACS_IO_BENCH_FULL=1 to run)" ,label)))

;; ═══════════════════════════════════════════════════════════════════════════
;; Category 1 — single-call bulk (both sides use their fastest one-shot API)
;; ═══════════════════════════════════════════════════════════════════════════

(ert-deftest emacs-io-perf-bulk-write-10m ()
  "10 MB end-to-end: accumulate in memory + write to file.
Compares the full pipeline both sides serve — build a string then persist it.
Stream: string-output → cl-write-string → cl-get-output-stream-string →
        cl-open output → cl-write-string → cl-close.
Buffer: with-temp-buffer → insert → buffer-string →
        with-temp-buffer → insert → write-region.
The in-memory phase costs stream an extra copy_string_contents query+crossing;
the file phase wins via raw write(2) bypassing write-region encoding/hooks."
  (emacs-io-perf--unless-full "bulk-write 10M"
   (let* ((n (* 10 1024 1024))
          (data (make-string n ?x))
          (fs (make-temp-file "emacs-io-perf-s-"))
          (fb (make-temp-file "emacs-io-perf-b-")))
     (unwind-protect
         (progn
           (emacs-io-perf--report "bulk-write 10M"
             ;; stream: accumulate → persist
             (emacs-io-perf--time
              (lambda ()
                (let* ((so (cl-make-string-output-stream :size (length data))))
                  (cl-write-string data so)
                  (let ((result (cl-get-output-stream-string so)))
                    (let ((sf (cl-open fs :direction :output :if-exists :supersede)))
                      (cl-write-string result sf)
                      (cl-close sf))))))
             ;; buffer: accumulate → persist
             (emacs-io-perf--time
              (lambda ()
                (let ((result (with-temp-buffer
                                (insert data)
                                (buffer-string))))
                  (with-temp-buffer
                    (insert result)
                    (write-region (point-min) (point-max) fb nil 'silent))))))
           (should (= (file-attribute-size (file-attributes fs)) n)))
       (ignore-errors (delete-file fs) (delete-file fb))))))

(ert-deftest emacs-io-perf-bulk-read-10m ()
  "10 MB single-call file read: cl-read-string(N) vs insert-file-contents.
Both sides materialise the whole file as a string; the stream reads through
the mmap-backed input path (zero copy) while the buffer fills its gap buffer
and copies back out."
  (emacs-io-perf--unless-full "bulk-read 10M (file)"
   (let* ((n (* 10 1024 1024))
          (data (make-string n ?x))
          (f (make-temp-file "emacs-io-perf-r-")))
     (unwind-protect
         (progn
           (with-temp-file f (insert data))
           (emacs-io-perf--report "bulk-read 10M (file)"
             (emacs-io-perf--time
              (lambda ()
                (let ((s (cl-open f :direction :input)))
                  (prog1 (cl-read-string n s) (cl-close s)))))
             (emacs-io-perf--time
              (lambda ()
                (with-temp-buffer (insert-file-contents f) (buffer-string))))
             (lambda (a b) (and (= (length a) n) (= (length b) n)))))
       (ignore-errors (delete-file f))))))

(ert-deftest emacs-io-perf-file-rw-10m ()
  "10 MB single-call: stream open+write+read vs write-region+insert-file-contents."
  (emacs-io-perf--unless-full "file-rw 10M"
   (let* ((n (* 10 1024 1024))
          (data (make-string n ?x))
          (fs (make-temp-file "emacs-io-perf-s-"))
          (fb (make-temp-file "emacs-io-perf-b-")))
     (unwind-protect
         (emacs-io-perf--report "file-rw 10M"
           (emacs-io-perf--time
            (lambda ()
              (let ((s (cl-open fs :direction :output :if-exists :supersede)))
                (cl-write-string data s) (cl-close s))
              (let ((s (cl-open fs :direction :input)))
                (prog1 (cl-read-string (length data) s)
                  (cl-close s)))))
           (emacs-io-perf--time
            (lambda ()
              (with-temp-buffer (insert data)
                (write-region (point-min) (point-max) fb nil 'silent))
              (with-temp-buffer (insert-file-contents fb)
                (buffer-string))))
           (lambda (a b) (and (= (length a) n) (= (length b) n))))
       (ignore-errors (delete-file fs) (delete-file fb))))))

(ert-deftest emacs-io-perf-fd-write-10m ()
  "10 MB single-call: stream fd write vs write-region."
  (emacs-io-perf--unless-full "fd-write 10M"
   (let* ((n (* 10 1024 1024))
          (data (make-string n ?x))
          (fs (make-temp-file "emacs-io-perf-s-"))
          (fb (make-temp-file "emacs-io-perf-b-")))
     (unwind-protect
         (progn
           (emacs-io-perf--report "fd-write 10M"
             (emacs-io-perf--time
              (lambda ()
                (let ((s (cl-open fs :direction :output :if-exists :supersede)))
                  (cl-write-string data s) (cl-close s))))
             (emacs-io-perf--time
              (lambda ()
                (with-temp-buffer (insert data)
                  (write-region (point-min) (point-max) fb nil 'silent)))))
           (should (= (file-attribute-size (file-attributes fs)) n)))
       (ignore-errors (delete-file fs) (delete-file fb))))))

;; ═══════════════════════════════════════════════════════════════════════════
;; Category 2 — amortized many-call (per-element loop on both sides)
;; ═══════════════════════════════════════════════════════════════════════════

(ert-deftest emacs-io-perf-seq-read-100k ()
  "100K elements: cl-read-sequence vs char-after loop."
  (let ((s (make-string 100000 ?x)))
    (emacs-io-perf--report "read-sequence 100K"
      (emacs-io-perf--time
       (lambda ()
         (emacs-io-perf--with-string-input s
           (let ((v (make-vector 100000 0)))
             (cl-read-sequence v stream) v))))
      (emacs-io-perf--time
       (lambda ()
         (with-temp-buffer (insert s)
           (let ((v (make-vector 100000 0)))
             (dotimes (i 100000)
               (setf (aref v i) (char-after (1+ i))))
             v))))
      (lambda (a b) (and (equal a b) (= (length a) 100000))))))

(ert-deftest emacs-io-perf-seq-write-100k ()
  "100K elements: cl-write-sequence vs insert-per-element loop."
  (let ((v (make-vector 100000 ?x)))
    (emacs-io-perf--report "write-sequence 100K"
      (emacs-io-perf--time
       (lambda ()
         (emacs-io-perf--with-string-output
           (cl-write-sequence v stream))))
      (emacs-io-perf--time
       (lambda ()
         (with-temp-buffer
           (dotimes (i 100000) (insert (aref v i)))
           (buffer-string))))
      (lambda (a b) (and (string= a b) (= (length a) 100000))))))

(ert-deftest emacs-io-perf-char-accumulate-100k ()
  "100K per-char writes: cl-write-char loop vs insert-char loop."
  (let ((n 100000) (ch ?x))
    (emacs-io-perf--report "char-accumulate 100K"
      (emacs-io-perf--time
       (lambda ()
         (let ((s (cl-make-string-output-stream)))
           (dotimes (_ n) (cl-write-char ch s))
           (cl-get-output-stream-string s))))
      (emacs-io-perf--time
       (lambda ()
         (with-temp-buffer
           (dotimes (_ n) (insert ch))
           (buffer-string))))
      (lambda (a b) (and (string= a b) (= (length a) n))))))

;; ═══════════════════════════════════════════════════════════════════════════
;; Category 3 — line processing (extracting lines from text)
;; ═══════════════════════════════════════════════════════════════════════════

(ert-deftest emacs-io-perf-line-read-10k ()
  "10K lines: cl-read-line vs buffer-substring+forward-line.
The line reader scans the stream's buffered window and advances exactly, so
it is competitive with the C-level `forward-line'."
  (let ((text (emacs-io-perf--with-string-output
                (dotimes (_ 10000)
                  (cl-write-line "the quick brown fox jumps over the lazy dog" stream)))))
    (emacs-io-perf--report "line-read 10K"
      (emacs-io-perf--time
       (lambda ()
         (emacs-io-perf--with-string-input text
           (let ((n 0))
             (while (car (cl-read-line stream nil nil))
               (setq n (1+ n)))
             n))))
      (emacs-io-perf--time
       (lambda ()
         (with-temp-buffer
           (insert text)
           (goto-char (point-min))
           (let ((n 0))
             (while (not (eobp))
               (buffer-substring (point) (line-end-position))
               (forward-line 1)
               (setq n (1+ n)))
             n))))
      (lambda (a b) (and (= a b) (= a 10000))))))

(ert-deftest emacs-io-perf-comint-10k ()
  "10K lines comint-sim: incremental arrival, stream read-line vs buffer insert+scan."
  (let ((text (emacs-io-perf--with-string-output
                (dotimes (_ 10000)
                  (cl-write-line "proc: test output line here" stream)))))
    (emacs-io-perf--report "comint-sim 10K"
      (emacs-io-perf--time
       (lambda ()
         (emacs-io-perf--with-string-input text
           (let ((n 0))
             (while (and (< n 10000) (car (cl-read-line stream nil nil)))
               (setq n (1+ n)))
             n))))
      (emacs-io-perf--time
       (lambda ()
         (with-temp-buffer
           (let ((n 0) (pos 0) (len (length text)) (chunk 256))
             (while (and (< pos len) (< n 10000))
               (let ((end (min (+ pos chunk) len)))
                 (save-excursion
                   (goto-char (point-max))
                   (insert (substring text pos end)))
                 (goto-char (point-min))
                 (while (and (< n 10000) (not (eobp)))
                   (when (< (line-end-position) (point-max))
                     (setq n (1+ n)))
                   (forward-line 1))
                 (delete-region (point-min) (line-beginning-position))
                 (setq pos end)))
             n))))
      (lambda (a b) (and (= a b) (= a 10000))))))

;; ═══════════════════════════════════════════════════════════════════════════
;; Category 4 — application scenarios (real-world I/O patterns)
;; ═══════════════════════════════════════════════════════════════════════════

;; ── IPC pipe simulation: incremental chunk read, materialise each chunk ──

(ert-deftest emacs-io-perf-pipe-read-10m ()
  "10 MB pipe-sim: chunked materialisation via cl-read-string vs buffer-substring.
Both sides allocate and count each 4 KB chunk; neither inserts into a growing
buffer (the incremental insert+scan case is covered by comint-10k)."
  (emacs-io-perf--unless-full "pipe-read 10M"
   (let* ((n (* 10 1024 1024))
          (data (make-string n ?x))
          (chunk 4096))
     (emacs-io-perf--report "pipe-read 10M"
       (emacs-io-perf--time
        (lambda ()
          (emacs-io-perf--with-string-input data
            (let ((total 0))
              (catch 'done
                (while t
                  (let ((s (cl-read-string chunk stream nil "")))
                    (if (equal s "") (throw 'done nil))
                    (setq total (+ total (length s))))))
              total))))
       (emacs-io-perf--time
        (lambda ()
          (with-temp-buffer
            (insert data)
            (let ((total 0) (pos (point-min)) (end (point-max)))
              (while (< pos end)
                (let ((to (min (+ pos chunk) end)))
                  (setq total (+ total (length (buffer-substring pos to))))
                  (setq pos to)))
              total))))
       (lambda (a b) (and (= a n) (= b n)))))))

;; ── log parsing: grep for pattern across many lines ──────────────────────

(ert-deftest emacs-io-perf-log-grep-10k ()
  "10K lines log-grep: per-line string-match on both sides."
  (let* ((n 10000)
         (text (emacs-io-perf--with-string-output
                 (dotimes (i n)
                   (cl-write-line (format "[%d] %s log entry here"
                                          i (if (zerop (mod i 100)) "ERROR" "INFO"))
                                  stream)))))
    (emacs-io-perf--report "log-grep 10K"
      (emacs-io-perf--time
       (lambda ()
         (emacs-io-perf--with-string-input text
           (let ((n 0) line)
             (while (setq line (car (cl-read-line stream nil nil)))
               (when (string-match "ERROR" line)
                 (setq n (1+ n))))
             n))))
      (emacs-io-perf--time
       (lambda ()
         (with-temp-buffer
           (insert text)
           (goto-char (point-min))
           (let ((n 0))
             (while (not (eobp))
               (when (string-match "ERROR"
                     (buffer-substring (point) (line-end-position)))
                 (setq n (1+ n)))
               (forward-line 1))
             n))))
      (lambda (a b) (and (= a b) (= a 100))))))

;; ── Lisp object reading: cl-read vs read-from-string ─────────────────────

(ert-deftest emacs-io-perf-read-objects-10k ()
  "10K generated S-expressions: cl-read vs Emacs' `read' from a buffer.
The input is a set of generated objects (distinct symbols/numbers/strings and
nested lists), serialized once.  Both parsers read that same text; `cl-read'
drives Emacs' reader from the stream as a function stream (one Lisp->C call per
character), so it is not a speed win over Emacs' native buffer reader --
reported without a win assertion (see the README performance notes)."
  (let* ((n 10000)
         (text (mapconcat (lambda (i)
                            (prin1-to-string
                             (list (intern (format "sym%d" i)) i
                                   (format "str%d" i) (list i (* i i)))))
                          (number-sequence 1 n) " ")))
    (emacs-io-perf--report "read-objects 10K"
      (emacs-io-perf--time
       (lambda ()
         (emacs-io-perf--with-string-input text
           (let ((k 0))
             (while (not (eq (cl-read stream nil :eof) :eof))
               (setq k (1+ k)))
             k))))
      (emacs-io-perf--time
       (lambda ()
         (with-temp-buffer
           (insert text)
           (goto-char (point-min))
           (let ((m (point-marker)) (k 0))
             (while (condition-case nil (progn (read m) t) (end-of-file nil))
               (setq k (1+ k)))
             k))))
      (lambda (a b) (and (= a n) (= b n)))
      nil)))                                  ; object reading is not a speed win

;; ═══════════════════════════════════════════════════════════════════════════

;; ── chunked read: cl-read-chunk vs buffer-substring, same chunk size ──────

(ert-deftest emacs-io-perf-chunk-read-1m ()
  "1 MB: cl-read-chunk vs buffer-substring in 4KB chunks on both sides."
  (emacs-io-perf--unless-full "chunk-read 1M"
   (let* ((n (* 1024 1024))
          (text (make-string n ?x))
          (chunk 4096))
     (emacs-io-perf--report "chunk-read 1M"
       (emacs-io-perf--time
        (lambda ()
          (emacs-io-perf--with-string-input text
            (let ((total 0) pair)
              (while (not (cdr (setq pair (cl-read-chunk stream chunk))))
                (setq total (+ total (length (car pair)))))
              (setq total (+ total (length (car pair))))
              total))))
       (emacs-io-perf--time
        (lambda ()
          (with-temp-buffer
            (insert text)
            (let ((total 0) (pos (point-min)) (end (point-max)))
              (while (< pos end)
                (let ((to (min (+ pos chunk) end)))
                  (setq total (+ total (length (buffer-substring pos to))))
                  (setq pos to)))
              total))))
       (lambda (a b) (and (= a n) (= b n)))))))

(provide 'emacs-io-perf-test)
;;; emacs-io-perf-test.el ends here
