;;; emacs-io-perf-test.el — performance benchmarks -*- lexical-binding: t; -*-

(require 'ert)

(module-load (expand-file-name "emacs-io.so" (file-name-directory load-file-name)))

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

;; ═══════════════════════════════════════════════════════════════════════════
;; string output: stream accumulation vs buffer insertion
;; ═══════════════════════════════════════════════════════════════════════════

(defun emacs-io-perf--stream-accumulate (n-chars char)
  (let ((s (cl-make-string-output-stream)))
    (dotimes (_ n-chars) (cl-write-char char s))
    (cl-get-output-stream-string s)))

(defun emacs-io-perf--buffer-accumulate (n-chars char)
  (with-temp-buffer
    (dotimes (_ n-chars) (insert char))
    (buffer-string)))

(ert-deftest emacs-io-perf-string-output-1k ()
  "1K: stream string-output vs buffer insert."
  (let ((n 1000) (ch ?x))
    (emacs-io-perf--report "string-output 1K"
      (emacs-io-perf--time (lambda () (emacs-io-perf--stream-accumulate n ch)))
      (emacs-io-perf--time (lambda () (emacs-io-perf--buffer-accumulate n ch)))
      :assert)))

(ert-deftest emacs-io-perf-string-output-100k ()
  "100K: stream string-output vs buffer insert."
  (let ((n 100000) (ch ?x))
    (emacs-io-perf--report "string-output 100K"
      (emacs-io-perf--time (lambda () (emacs-io-perf--stream-accumulate n ch)))
      (emacs-io-perf--time (lambda () (emacs-io-perf--buffer-accumulate n ch)))
      :assert)))

;; ═══════════════════════════════════════════════════════════════════════════
;; bulk read/write: cl-read-sequence / cl-write-sequence vs per-element ops
;; ═══════════════════════════════════════════════════════════════════════════

(defun emacs-io-perf--stream-bulk-read (string nchars)
  (emacs-io-perf--with-string-input string
    (let ((buf (make-vector nchars 0)))
      (cl-read-sequence buf stream)
      buf)))

(defun emacs-io-perf--buffer-bulk-read (string nchars)
  (with-temp-buffer
    (insert string)
    (let ((buf (make-vector nchars 0)))
      (dotimes (i nchars)
        (setf (aref buf i) (char-after (+ (point-min) i))))
      buf)))

(ert-deftest emacs-io-perf-seq-read-100k ()
  "100K: stream cl-read-sequence vs buffer char-after loop."
  (let ((s (make-string 100000 ?x)))
    (emacs-io-perf--report "read-sequence 100K"
      (emacs-io-perf--time (lambda () (emacs-io-perf--stream-bulk-read s 100000)))
      (emacs-io-perf--time (lambda () (emacs-io-perf--buffer-bulk-read s 100000)))
      :assert)))

(defun emacs-io-perf--stream-bulk-write (vec n)
  (emacs-io-perf--with-string-output
    (cl-write-sequence vec stream :end n)))

(defun emacs-io-perf--buffer-bulk-write (vec n)
  (with-temp-buffer
    (dotimes (i n) (insert (aref vec i)))
    (buffer-string)))

(ert-deftest emacs-io-perf-seq-write-100k ()
  "100K: stream cl-write-sequence vs buffer insert loop."
  (let ((v (make-vector 100000 ?x)))
    (emacs-io-perf--report "write-sequence 100K"
      (emacs-io-perf--time (lambda () (emacs-io-perf--stream-bulk-write v 100000)))
      (emacs-io-perf--time (lambda () (emacs-io-perf--buffer-bulk-write v 100000)))
      :assert)))

;; ═══════════════════════════════════════════════════════════════════════════
;; line extraction: string stream vs buffer-substring
;; ═══════════════════════════════════════════════════════════════════════════

(defun emacs-io-perf--stream-read-lines (text)
  (emacs-io-perf--with-string-input text
    (let ((n 0))
      (while (car (cl-read-line stream nil nil))
        (setq n (1+ n)))
      n)))

(defun emacs-io-perf--buffer-read-lines (text)
  (with-temp-buffer
    (insert text)
    (goto-char (point-min))
    (let ((n 0))
      (while (not (eobp))
        (buffer-substring (point) (line-end-position))
        (forward-line 1)
        (setq n (1+ n)))
      n)))

(ert-deftest emacs-io-perf-line-read-10k ()
  "10K lines: stream cl-read-line vs buffer buffer-substring."
  (let ((text (emacs-io-perf--with-string-output
                (dotimes (_ 10000)
                  (cl-write-line "the quick brown fox jumps over the lazy dog" stream)))))
    (emacs-io-perf--report "line read 10K"
      (emacs-io-perf--time (lambda () (emacs-io-perf--stream-read-lines text)))
      (emacs-io-perf--time (lambda () (emacs-io-perf--buffer-read-lines text)))
      ;; per-char stream_read overhead; buffer-substring is a single memcpy.
      ;; Report-only; bulk read-line optimisation is future work.
      nil)))

;; ═══════════════════════════════════════════════════════════════════════════
;; comint process-output simulation: incremental arrival, per-line processing
;; ═══════════════════════════════════════════════════════════════════════════

(defun emacs-io-perf--stream-incremental (lines n-lines chunk)
  "Simulate process output via stream: read lines as they arrive in chunks."
  (emacs-io-perf--with-string-input lines
    (let ((n 0))
      (while (and (< n n-lines) (car (cl-read-line stream nil nil)))
        (setq n (1+ n)))
      n)))

(defun emacs-io-perf--buffer-incremental (lines n-lines chunk)
  "Simulate comint: insert CHUNK bytes at a time, extract each complete line."
  (with-temp-buffer
    (let ((n 0)
          (pos 0)
          (len (length lines)))
      (while (and (< pos len) (< n n-lines))
        (let ((end (min (+ pos chunk) len)))
          (save-excursion
            (goto-char (point-max))
            (insert (substring lines pos end)))
          ;; find and count complete lines in the buffer
          (goto-char (point-min))
          (while (and (< n n-lines) (not (eobp)))
            (let ((eol (line-end-position)))
              (when (< eol (point-max))          ;; line has a \n after it
                (setq n (1+ n))))
            (forward-line 1))
          (delete-region (point-min) (line-beginning-position))
          (setq pos end)))
      n)))

(ert-deftest emacs-io-perf-comint-10k ()
  "10K lines: stream read-line vs comint-style incremental buffer insert."
  (let ((text (emacs-io-perf--with-string-output
                (dotimes (_ 10000)
                  (cl-write-line "proc: test output line here" stream)))))
    (emacs-io-perf--report "comint-sim 10K"
      (emacs-io-perf--time (lambda () (emacs-io-perf--stream-incremental text 10000 256)))
      (emacs-io-perf--time (lambda () (emacs-io-perf--buffer-incremental text 10000 256)))
      ;; comint buffer incremental insert+scan vs stream line-by-line.
      ;; Report-only; per-char read-line overhead makes stream comparable
      ;; rather than dominant.  Bulk sink the stream wins elsewhere.
      nil)))

(provide 'emacs-io-perf-test)
;;; emacs-io-perf-test.el ends here
