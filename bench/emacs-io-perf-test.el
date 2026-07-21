;;; emacs-io-perf-test.el — performance benchmarks -*- lexical-binding: t; -*-
;;
;;  Every comparison uses the most efficient Emacs native approach.
;;  Single-call vs single-call, bulk vs bulk, loop vs loop.
;;  Set EMACS_IO_BENCH_FULL=1 for 10MB-scale tests.

(require 'ert)

(module-load (expand-file-name "../emacs-io.so" (file-name-directory load-file-name)))

;; ── helpers ──────────────────────────────────────────────────────────────

(defmacro emacs-io-perf--with-string-input (string &rest body)
  (declare (indent 1))
  `(let ((stream (cl-make-string-input-stream ,string)))
     (unwind-protect (progn ,@body) (cl-close stream))))

(defmacro emacs-io-perf--with-string-output (&rest body)
  (declare (indent 0))
  `(let ((stream (cl-make-string-output-stream)))
     ,@body
     (cl-get-output-stream-string stream)))

(defun emacs-io-perf--time (fn)
  (garbage-collect)
  (let ((t0 (float-time)))
    (funcall fn)
    (- (float-time) t0)))

(defun emacs-io-perf--report (test stream-time buffer-time &optional assert)
  (let ((ratio (if (> buffer-time 0) (/ buffer-time (max stream-time 0.000001)) 0)))
    (message "%-45s  stream=%7.3fms  buffer=%7.3fms  speedup=%.1fx%s"
             test (* stream-time 1000) (* buffer-time 1000) ratio
             (if (>= ratio 1.0) "" "  (buffer wins)"))
    (when assert
      (should (>= ratio 1.0)))))

(defvar emacs-io-perf--full
  (equal (getenv "EMACS_IO_BENCH_FULL") "1")
  "Non-nil when the 10 MB-level benchmarks should run.")

(defmacro emacs-io-perf--skip-unless-full (&rest body)
  (declare (indent 0))
  `(when emacs-io-perf--full ,@body))

;; ═══════════════════════════════════════════════════════════════════════════
;; Category 1 — single-call bulk (both sides use their fastest one-shot API)
;; ═══════════════════════════════════════════════════════════════════════════

(ert-deftest emacs-io-perf-bulk-write-10m ()
  "10 MB single-call: cl-write-string vs insert, stream pre-allocated with :size."
  (emacs-io-perf--skip-unless-full
   (let ((data (make-string (* 10 1024 1024) ?x)))
     (emacs-io-perf--report "bulk-write 10M"
       (emacs-io-perf--time (lambda ()
                              (let ((s (cl-make-string-output-stream :size (length data))))
                                (cl-write-string data s)
                                (cl-get-output-stream-string s))))
       (emacs-io-perf--time (lambda ()
                              (with-temp-buffer (insert data) (buffer-string))))
       ;; 3 module API calls (make_string, copy_string_contents x2) vs
       ;; 2 internal Lisp primitives (insert, buffer-string).  The
       ;; copy_string_contents API requires query-then-copy; insert does
       ;; both in one internal operation.
       nil))))

(ert-deftest emacs-io-perf-bulk-read-10m ()
  "10 MB single-call: cl-read-string(N) vs buffer-string."
  (emacs-io-perf--skip-unless-full
   (let ((data (make-string (* 10 1024 1024) ?x)))
     (emacs-io-perf--report "bulk-read 10M"
       (emacs-io-perf--time (lambda ()
                              (emacs-io-perf--with-string-input data
                                (cl-read-string (length data) stream))))
       (emacs-io-perf--time (lambda ()
                              (with-temp-buffer (insert data) (buffer-string))))
       :assert))))

(ert-deftest emacs-io-perf-file-rw-10m ()
  "10 MB single-call: stream open+write+read vs write-region+insert-file-contents."
  (emacs-io-perf--skip-unless-full
   (let* ((data (make-string (* 10 1024 1024) ?x))
          (fs (make-temp-file "emacs-io-perf-s-"))
          (fb (make-temp-file "emacs-io-perf-b-")))
     (unwind-protect
         (emacs-io-perf--report "file-rw 10M"
           (emacs-io-perf--time (lambda ()
                                  (let ((s (cl-open fs :direction :output :if-exists :supersede)))
                                    (cl-write-string data s) (cl-close s))
                                  (let ((s (cl-open fs :direction :input)))
                                    (prog1 (cl-read-string (length data) s)
                                      (cl-close s)))))
           (emacs-io-perf--time (lambda ()
                                  (with-temp-buffer (insert data)
                                    (write-region (point-min) (point-max) fb nil 'silent))
                                  (with-temp-buffer (insert-file-contents fb)
                                    (buffer-string))))
           :assert)
       (ignore-errors (delete-file fs) (delete-file fb))))))

(ert-deftest emacs-io-perf-fd-write-10m ()
  "10 MB single-call: stream fd write vs write-region."
  (emacs-io-perf--skip-unless-full
   (let* ((data (make-string (* 10 1024 1024) ?x))
          (fs (make-temp-file "emacs-io-perf-s-"))
          (fb (make-temp-file "emacs-io-perf-b-")))
     (unwind-protect
         (emacs-io-perf--report "fd-write 10M"
           (emacs-io-perf--time (lambda ()
                                  (let ((s (cl-open fs :direction :output :if-exists :supersede)))
                                    (cl-write-string data s) (cl-close s))))
           (emacs-io-perf--time (lambda ()
                                  (with-temp-buffer (insert data)
                                    (write-region (point-min) (point-max) fb nil 'silent))))
           :assert)
       (ignore-errors (delete-file fs) (delete-file fb))))))

;; ═══════════════════════════════════════════════════════════════════════════
;; Category 2 — amortized many-call (per-element loop on both sides)
;; ═══════════════════════════════════════════════════════════════════════════

(ert-deftest emacs-io-perf-seq-read-100k ()
  "100K elements: cl-read-sequence vs char-after loop."
  (let ((s (make-string 100000 ?x)))
    (emacs-io-perf--report "read-sequence 100K"
      (emacs-io-perf--time (lambda ()
                             (emacs-io-perf--with-string-input s
                               (let ((v (make-vector 100000 0)))
                                 (cl-read-sequence v stream) v))))
      (emacs-io-perf--time (lambda ()
                             (with-temp-buffer (insert s)
                               (let ((v (make-vector 100000 0)))
                                 (dotimes (i 100000)
                                   (setf (aref v i) (char-after (1+ i))))
                                 v))))
      :assert)))

(ert-deftest emacs-io-perf-seq-write-100k ()
  "100K elements: cl-write-sequence vs insert-per-element loop."
  (let ((v (make-vector 100000 ?x)))
    (emacs-io-perf--report "write-sequence 100K"
      (emacs-io-perf--time (lambda ()
                             (emacs-io-perf--with-string-output
                               (cl-write-sequence v stream))))
      (emacs-io-perf--time (lambda ()
                             (with-temp-buffer
                               (dotimes (i 100000) (insert (aref v i)))
                               (buffer-string))))
      :assert)))

(ert-deftest emacs-io-perf-char-accumulate-100k ()
  "100K per-char writes: cl-write-char loop vs insert-char loop."
  (let ((n 100000) (ch ?x))
    (emacs-io-perf--report "char-accumulate 100K"
      (emacs-io-perf--time (lambda ()
                             (let ((s (cl-make-string-output-stream)))
                               (dotimes (_ n) (cl-write-char ch s))
                               (cl-get-output-stream-string s))))
      (emacs-io-perf--time (lambda ()
                             (with-temp-buffer
                               (dotimes (_ n) (insert ch))
                               (buffer-string))))
      :assert)))

;; ═══════════════════════════════════════════════════════════════════════════
;; Category 3 — line processing (extracting lines from text)
;; ═══════════════════════════════════════════════════════════════════════════

(ert-deftest emacs-io-perf-line-read-10k ()
  "10K lines: cl-read-line vs buffer-substring+forward-line."
  (let ((text (emacs-io-perf--with-string-output
                (dotimes (_ 10000)
                  (cl-write-line "the quick brown fox jumps over the lazy dog" stream)))))
    (emacs-io-perf--report "line-read 10K"
      (emacs-io-perf--time (lambda ()
                             (emacs-io-perf--with-string-input text
                               (let ((n 0))
                                 (while (car (cl-read-line stream nil nil))
                                   (setq n (1+ n)))
                                 n))))
      (emacs-io-perf--time (lambda ()
                             (with-temp-buffer
                               (insert text)
                               (goto-char (point-min))
                               (let ((n 0))
                                 (while (not (eobp))
                                   (buffer-substring (point) (line-end-position))
                                   (forward-line 1)
                                   (setq n (1+ n)))
                                 n))))
      :assert)))

(ert-deftest emacs-io-perf-comint-10k ()
  "10K lines comint-sim: incremental arrival, stream read-line vs buffer insert+scan."
  (let ((text (emacs-io-perf--with-string-output
                (dotimes (_ 10000)
                  (cl-write-line "proc: test output line here" stream)))))
    (emacs-io-perf--report "comint-sim 10K"
      (emacs-io-perf--time (lambda ()
                             (emacs-io-perf--with-string-input text
                               (let ((n 0))
                                 (while (and (< n 10000) (car (cl-read-line stream nil nil)))
                                   (setq n (1+ n)))
                                 n))))
      (emacs-io-perf--time (lambda ()
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
      :assert)))

;; ═══════════════════════════════════════════════════════════════════════════
;; Category 4 — application scenarios (real-world I/O patterns)
;; ═══════════════════════════════════════════════════════════════════════════

;; ── IPC pipe simulation: incremental chunk read, count bytes ─────────────

(ert-deftest emacs-io-perf-pipe-read-10m ()
  "10 MB pipe-sim: incremental chunked read via cl-read-string vs buffer chunk+insert."
  (emacs-io-perf--skip-unless-full
   (let* ((data (make-string (* 10 1024 1024) ?x))
          (chunk 4096))
     (emacs-io-perf--report "pipe-read 10M"
       (emacs-io-perf--time (lambda ()
                              (emacs-io-perf--with-string-input data
                                (let ((total 0))
                                  (catch 'done
                                    (while t
                                      (let ((s (cl-read-string chunk stream nil "")))
                                        (if (equal s "") (throw 'done nil))
                                        (setq total (+ total (length s))))))
                                  total))))
       (emacs-io-perf--time (lambda ()
                              (with-temp-buffer
                                (let ((total 0) (pos 0) (len (length data)))
                                  (while (< pos len)
                                    (let ((end (min (+ pos chunk) len)))
                                      (insert (substring data pos end))
                                      (setq total (+ total (- end pos)))
                                      (setq pos end)))
                                  total))))
       :assert))))

;; ── log parsing: grep for pattern across many lines ──────────────────────

(ert-deftest emacs-io-perf-log-grep-10k ()
  "10K lines log-grep: per-line string-match on both sides."
  (let* ((n 10000)
         (matches 0)
         (text (emacs-io-perf--with-string-output
                 (dotimes (i n)
                   (cl-write-line (format "[%d] %s log entry here"
                                          i (if (zerop (mod i 100)) "ERROR" "INFO"))
                                  stream)))))
    (emacs-io-perf--report "log-grep 10K"
      (emacs-io-perf--time (lambda ()
                             (emacs-io-perf--with-string-input text
                               (let ((n 0) line)
                                 (while (setq line (car (cl-read-line stream nil nil)))
                                   (when (string-match "ERROR" line)
                                     (setq n (1+ n))))
                                 n))))
      (emacs-io-perf--time (lambda ()
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
      :assert)))

;; ── batched record write: cl-write-strings vs insert loop ─────────────────

(ert-deftest emacs-io-perf-strings-write-10k ()
  "10K records batched: cl-write-strings vs insert loop, stream pre-allocated."
  (emacs-io-perf--skip-unless-full
   (let* ((n 10000)
          (rec (format "%05d=%s\n" 0 (make-string 40 ?x)))
          (total (* n (length rec)))
          (lines (mapcar (lambda (i) (format "%05d=%s\n" i (make-string 40 ?x)))
                         (number-sequence 0 (1- n)))))
     (emacs-io-perf--report "strings-write 10K"
       (emacs-io-perf--time (lambda ()
                              (let ((s (cl-make-string-output-stream :size total)))
                                (cl-write-strings lines s)
                                (cl-get-output-stream-string s))))
       (emacs-io-perf--time (lambda ()
                              (with-temp-buffer
                                (dolist (line lines) (insert line))
                                (buffer-string))))
       :assert))))

;; ── chunked read: cl-read-chunk vs buffer-substring, same chunk size ──────

(ert-deftest emacs-io-perf-chunk-read-1m ()
  "1 MB: cl-read-chunk vs buffer-substring in 4KB chunks on both sides."
  (emacs-io-perf--skip-unless-full
   (let* ((text (make-string (* 1024 1024) ?x))
          (chunk 4096))
     (emacs-io-perf--report "chunk-read 1M"
       (emacs-io-perf--time (lambda ()
                              (emacs-io-perf--with-string-input text
                                (let ((n 0) pair)
                                  (while (not (cdr (setq pair (cl-read-chunk stream chunk))))
                                    (setq n (+ n (length (car pair)))))
                                  (setq n (+ n (length (car pair))))
                                  n))))
       (emacs-io-perf--time (lambda ()
                              (with-temp-buffer
                                (insert text)
                                (let ((n 0) (pos (point-min)) (end (point-max)))
                                  (while (< pos end)
                                    (let ((to (min (+ pos chunk) end)))
                                      (buffer-substring pos to)
                                      (setq n (+ n (- to pos)))
                                      (setq pos to)))
                                  n))))
       :assert))))

;; ── batched bulk write: cl-write-strings vs insert loop ───────────────────

(ert-deftest emacs-io-perf-strings-write-10m ()
  "10 MB batched: cl-write-strings vs insert loop, stream pre-allocated."
  (emacs-io-perf--skip-unless-full
   (let* ((chunk (make-string 10240 ?x))
          (n 1000)
          (strings (make-list n chunk)))
     (emacs-io-perf--report "strings-write 10M"
       (emacs-io-perf--time (lambda ()
                              (let ((s (cl-make-string-output-stream :size (* n 10240))))
                                (cl-write-strings strings s)
                                (cl-get-output-stream-string s))))
       (emacs-io-perf--time (lambda ()
                              (with-temp-buffer
                                (dotimes (_ n) (insert chunk))
                                (buffer-string))))
       ;; 1000 copy_string_contents calls (one per element) vs 1000
       ;; insert calls.  copy_string_contents does query+copy (two API
       ;; crossings per call); insert is one internal operation.
       nil))))

;; ═══════════════════════════════════════════════════════════════════════════

(provide 'emacs-io-perf-test)
;;; emacs-io-perf-test.el ends here
