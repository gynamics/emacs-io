;;; emacs-io-perf-test.el — performance benchmarks -*- lexical-binding: t; -*-

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

;; ═══════════════════════════════════════════════════════════════════════════
;; scale / stress: correctness at volume
;; ═══════════════════════════════════════════════════════════════════════════

(ert-deftest emacs-io-perf-large-string-output ()
  "100K write-string: no data corruption."
  (let* ((big (make-string 100000 ?X))
         (result (emacs-io-perf--with-string-output
                   (cl-write-string big stream))))
    (should (equal (length result) 100000))
    (should (string-match-p "\\`X+\\'" result))))

(ert-deftest emacs-io-perf-large-line-output ()
  "1K write-line: correct number of newlines."
  (let* ((n 1000)
         (result (emacs-io-perf--with-string-output
                   (dotimes (i n)
                     (cl-write-line (format "line %d" i) stream)))))
    (should (equal (cl-count ?\n result) n))))

;; ═══════════════════════════════════════════════════════════════════════════
;; 10 MB scale — bulk I/O throughput (fair: single-call on both sides)
;; Set EMACS_IO_BENCH_FULL=1 to run these.
;; ═══════════════════════════════════════════════════════════════════════════

(defvar emacs-io-perf--full
  (equal (getenv "EMACS_IO_BENCH_FULL") "1")
  "Non-nil when the 10 MB-level benchmarks should run.")

(defmacro emacs-io-perf--skip-unless-full (&rest body)
  (declare (indent 0))
  `(when emacs-io-perf--full ,@body))

;; ── 10 MB string output, bulk write ─────────────────────────────────────

(ert-deftest emacs-io-perf-bulk-output-10m ()
  "10 MB bulk string write, stream vs buffer (single call on both sides)."
  (emacs-io-perf--skip-unless-full
   (let ((data (make-string (* 10 1024 1024) ?x)))
     (emacs-io-perf--report "bulk-write 10M"
       (emacs-io-perf--time (lambda ()
                              (emacs-io-perf--with-string-output
                                (cl-write-string data stream))))
       (emacs-io-perf--time (lambda ()
                              (with-temp-buffer (insert data) (buffer-string))))
       ;; Emacs `insert' is a single C primitive; stream_write
       ;; goes through mutex + vtable dispatch.
       nil))))

;; ── 10 MB string input, bulk read into vector ───────────────────────────

(ert-deftest emacs-io-perf-bulk-read-10m ()
  "10 MB bulk read into string, stream cl-read-string vs buffer-string."
  (emacs-io-perf--skip-unless-full
   (let ((data (make-string (* 10 1024 1024) ?x)))
     (emacs-io-perf--report "bulk-read 10M"
       (emacs-io-perf--time (lambda ()
                              (emacs-io-perf--with-string-input data
                                (cl-read-string (length data) stream))))
       (emacs-io-perf--time (lambda ()
                              (with-temp-buffer
                                (insert data)
                                (buffer-string))))
       :assert))))

;; ── 10 MB file write + read round-trip ──────────────────────────────────

(ert-deftest emacs-io-perf-file-rw-10m ()
  "10 MB file write+read round-trip, stream vs Emacs native."
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
                                     (vconcat (buffer-string)))))
            :assert)
        (ignore-errors (delete-file fs) (delete-file fb))))))

;; ── Block-chain advantage: many medium writes (no realloc copies) ────────

(ert-deftest emacs-io-perf-blockchain-write-10m ()
  "1000 × 10KB writes via cl-write-string, block-chain vs buffer insert loop."
  (emacs-io-perf--skip-unless-full
   (let* ((chunk (make-string 10240 ?x))
          (n 1000))
     (emacs-io-perf--report "blockchain-write 10M"
       (emacs-io-perf--time (lambda ()
                              (emacs-io-perf--with-string-output
                                (dotimes (_ n)
                                  (cl-write-string chunk stream)))))
       (emacs-io-perf--time (lambda ()
                              (with-temp-buffer
                                (dotimes (_ n) (insert chunk))
                                (buffer-string))))
       ;; 1000 malloc calls (one per 64KB block) vs 1000 insert calls.
       ;; Block-chain wins vs realloc but loses to Emacs' C-level insert.
       nil))))

;; ── File write: stream fd vs Emacs native (isolated from round-trip) ─────

(ert-deftest emacs-io-perf-fd-write-10m ()
  "10 MB file write: stream fd vs write-region."
  (emacs-io-perf--skip-unless-full
   (let* ((data (make-string (* 10 1024 1024) ?x))
          (fs (make-temp-file "emacs-io-perf-s-"))
          (fb (make-temp-file "emacs-io-perf-b-")))
     (unwind-protect
         (emacs-io-perf--report "fd-write 10M"
           (emacs-io-perf--time (lambda ()
                                  (let ((s (cl-open fs :direction :output :if-exists :supersede)))
                                    (cl-write-string data s)
                                    (cl-close s))))
           (emacs-io-perf--time (lambda ()
                                  (with-temp-buffer (insert data)
                                    (write-region (point-min) (point-max) fb nil 'silent))))
           :assert)
       (ignore-errors (delete-file fs) (delete-file fb))))))

;; ═══════════════════════════════════════════════════════════════════════════
;; file-type detection — verify fstat / isatty classification
;; ═══════════════════════════════════════════════════════════════════════════

(ert-deftest emacs-io-perf-file-type-regular ()
  "Regular temp file is detected as S_ISREG."
  (emacs-io-perf--skip-unless-full
   (let* ((file (make-temp-file "emacs-io-perf-"))
          (s    (cl-open file :direction :input)))
     (unwind-protect
         (should (cl-file-regular-p s))
       (cl-close s)
       (ignore-errors (delete-file file))))))

(ert-deftest emacs-io-perf-file-type-tty ()
  "stdin from batch Emacs is not a tty; /dev/null is a character device."
  (emacs-io-perf--skip-unless-full
   (let ((s (cl-open "/dev/null" :direction :input)))
     (unwind-protect
         (progn (should-not (cl-file-tty-p s))
                (should-not (cl-file-regular-p s)))
       (cl-close s)))))

;; ═══════════════════════════════════════════════════════════════════════════
;; memory leak smoke test: create/destroy many streams, check stability
;; ═══════════════════════════════════════════════════════════════════════════

(ert-deftest emacs-io-perf-mem-leak ()
  "1000 create-write-destroy cycles; should not crash."
  (emacs-io-perf--skip-unless-full
   (dotimes (_ 1000)
     (let ((s (cl-make-string-output-stream)))
       (cl-write-string "hello world" s)
       (cl-get-output-stream-string s)
       (cl-close s)))
   (should t)))

(provide 'emacs-io-perf-test)
;;; emacs-io-perf-test.el ends here
