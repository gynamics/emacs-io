;;; emacs-io-conformance-test.el --- CL conformance tests -*- lexical-binding: t; -*-
;;
;; Boundary-case tests for the cl-* stream interfaces, written against the
;; ANSI Common Lisp specification (CLHS).

(require 'ert)

(add-to-list 'load-path (expand-file-name ".." (file-name-directory load-file-name)))
(require 'emacs-io)

;; emacs-io sets these with `set', so under lexical-binding they are not known
;; to be special.  Declare them so `let'-binding them is visible to
;; `symbol-value' (which the module uses internally).
(defvar *standard-input*)
(defvar *standard-output*)
(defvar *error-output*)
(defvar *query-io*)

;; ── helpers ──────────────────────────────────────────────────────────────

(defmacro cl-conf--in (string &rest body)
  "Run BODY with a string input stream over STRING."
  (declare (indent 1))
  `(let ((s (cl-make-string-input-stream ,string)))
     (unwind-protect (progn ,@body) (cl-close s))))

(defmacro cl-conf--out (&rest body)
  "Run BODY with a string output stream; return accumulated string."
  `(let ((s (cl-make-string-output-stream)))
     (prog1 (progn ,@body (cl-get-output-stream-string s))
       (cl-close s))))

(defmacro cl-conf--tmp (var &rest body)
  (declare (indent 1))
  `(let ((,var (make-temp-file "cl-conf-")))
     (unwind-protect (progn ,@body) (ignore-errors (delete-file ,var)))))

;; ── READ-CHAR ─────────────────────────────────────────────────────────────

(ert-deftest cl-conf-read-char-basic ()
  (cl-conf--in "abc"
    (should (equal (cl-read-char s) ?a))
    (should (equal (cl-read-char s) ?b))
    (should (equal (cl-read-char s) ?c))))

(ert-deftest cl-conf-read-char-eof-signals-by-default ()
  ;; CL: eof-error-p defaults to true -> end-of-file.
  (cl-conf--in ""
    (should-error (cl-read-char s) :type 'end-of-file)))

(ert-deftest cl-conf-read-char-eof-value ()
  ;; CL: eof-error-p nil -> returns eof-value (default nil).
  (cl-conf--in ""
    (should (null (cl-read-char s nil)))
    (should (eq (cl-read-char s nil :the-end) :the-end))))

(ert-deftest cl-conf-read-char-recursive-p-ignored ()
  ;; CL: recursive-p is accepted (t here) and does not affect the result.
  (cl-conf--in "z" (should (equal (cl-read-char s nil nil t) ?z))))

;; ── READ-LINE ─────────────────────────────────────────────────────────────

(ert-deftest cl-conf-read-line-newline-terminated ()
  (cl-conf--in "ab\ncd"
    (let ((r (cl-read-line s)))
      ;; CL: line = "ab", missing-newline-p = false
      (should (equal (car r) "ab"))
      (should (null (cdr r))))))

(ert-deftest cl-conf-read-line-eof-terminated ()
  ;; CL: missing-newline-p is true when the line ends at EOF.
  (cl-conf--in "abc"
    (let ((r (cl-read-line s)))
      (should (equal (car r) "abc"))
      (should (cdr r)))))

(ert-deftest cl-conf-read-line-immediate-eof-second-value ()
  "CL: (read-line in nil :eof) => :eof, true (two values)."
  (cl-conf--in ""
    (should (equal (cl-read-line s nil :eof) (cons :eof t)))))

(ert-deftest cl-conf-read-line-empty-line ()
  (cl-conf--in "\nx"
    (let ((r (cl-read-line s)))
      (should (equal (car r) ""))
      (should (null (cdr r))))))

(ert-deftest cl-conf-read-line-crlf ()
  (cl-conf--in "a\r\nb"
    (should (equal (car (cl-read-line s)) "a"))
    (should (equal (car (cl-read-line s)) "b"))))

;; ── READ-BYTE ─────────────────────────────────────────────────────────────

(ert-deftest cl-conf-read-byte-basic ()
  (cl-conf--in "ab"
    (should (equal (cl-read-byte s) 97))
    (should (equal (cl-read-byte s) 98))))

(ert-deftest cl-conf-read-byte-eof ()
  (cl-conf--in ""
    (should-error (cl-read-byte s) :type 'end-of-file)
    (should (eq (cl-read-byte s nil :eof) :eof))))

;; ── READ-CHAR-NO-HANG ─────────────────────────────────────────────────────

(ert-deftest cl-conf-read-char-no-hang-available ()
  (cl-conf--in "x" (should (equal (cl-read-char-no-hang s) ?x))))

(ert-deftest cl-conf-read-char-no-hang-eof-signals ()
  ;; CL: a non-interactive stream at EOF signals with the default eof-error-p.
  (cl-conf--in "" (should-error (cl-read-char-no-hang s) :type 'end-of-file)))

(ert-deftest cl-conf-read-char-no-hang-eof-value ()
  (cl-conf--in "" (should (eq (cl-read-char-no-hang s nil :eof) :eof))))

;; ── PEEK-CHAR ─────────────────────────────────────────────────────────────

(ert-deftest cl-conf-peek-char-nil ()
  (cl-conf--in "ab"
    (should (equal (cl-peek-char nil s) ?a))
    (should (equal (cl-read-char s) ?a))))

(ert-deftest cl-conf-peek-char-skip-whitespace ()
  ;; CL: peek-type t skips whitespace and leaves the next char.
  (cl-conf--in "  \n\tx"
    (should (equal (cl-peek-char t s) ?x))
    (should (equal (cl-read-char s) ?x))))

(ert-deftest cl-conf-peek-char-char-leaves-match ()
  "CL: char peek-type stops at the matched char and leaves it."
  ;; CLHS example: (peek-char #\\4 s) on \"    1 2 3 4 5\" => #\\4
  (cl-conf--in "    1 2 3 4 5"
    (should (equal (cl-peek-char t s) ?1))
    (should (equal (cl-peek-char ?4 s) ?4))
    (should (equal (cl-peek-char nil s) ?4))))

(ert-deftest cl-conf-peek-char-skip-to-first-match ()
  (cl-conf--in "aaaab"
    ;; first char equals the target -> returned, not consumed
    (should (equal (cl-peek-char ?a s) ?a))
    (should (equal (cl-read-char s) ?a))))

(ert-deftest cl-conf-peek-char-eof-signals-by-default ()
  (cl-conf--in "" (should-error (cl-peek-char nil s) :type 'end-of-file)))

(ert-deftest cl-conf-peek-char-eof-value ()
  (cl-conf--in "" (should (eq (cl-peek-char nil s nil :eof) :eof))))

(ert-deftest cl-conf-peek-char-recursive-p-accepted ()
  ;; CL signature has 5 optional args; the 5th (recursive-p) must be accepted.
  (cl-conf--in "q" (should (equal (cl-peek-char nil s nil nil t) ?q))))

;; ── UNREAD-CHAR / LISTEN ──────────────────────────────────────────────────

(ert-deftest cl-conf-unread-char ()
  (cl-conf--in "ab"
    (should (equal (cl-read-char s) ?a))
    (cl-unread-char ?a s)
    (should (equal (cl-read-char s) ?a))
    (should (equal (cl-read-char s) ?b))))

(ert-deftest cl-conf-listen ()
  (cl-conf--in "x"
    (should (cl-listen s))
    (cl-read-char s)
    (should-not (cl-listen s))))

;; ── READ-SEQUENCE / WRITE-SEQUENCE ────────────────────────────────────────

(ert-deftest cl-conf-read-sequence-vector ()
  (cl-conf--in "ABCDE"
    (let ((v (make-vector 5 0)))
      (should (equal (cl-read-sequence v s) 5))
      (should (equal (aref v 0) 65)))))

(ert-deftest cl-conf-read-sequence-partial-returns-index ()
  (cl-conf--in "ab"
    (let ((v (make-vector 5 0)))
      (should (equal (cl-read-sequence v s) 2))
      (should (equal (aref v 2) 0)))))

(ert-deftest cl-conf-read-sequence-start-end ()
  (cl-conf--in "abcdefgh"
    (let ((v (make-vector 8 0)))
      (cl-read-sequence v s :start 2 :end 6)
      (should (equal (aref v 2) ?a))
      (should (equal (aref v 6) 0)))))

(ert-deftest cl-conf-read-sequence-string ()
  (cl-conf--in "abc"
    (let ((str (make-string 3 ?\ )))
      (should (equal (cl-read-sequence str s) 3))
      (should (equal str "abc")))))

(ert-deftest cl-conf-read-sequence-list ()
  (cl-conf--in "abc"
    (let ((lst (list 0 0 0)))
      (should (equal (cl-read-sequence lst s) 3))
      (should (equal lst (list ?a ?b ?c))))))

(ert-deftest cl-conf-read-sequence-bad-start-signals ()
  "CL: start must be a valid bounding index (an out-of-range error)."
  (cl-conf--in "abc"
    (should-error (cl-read-sequence (make-vector 3 0) s :start 99)
                  :type 'args-out-of-range)
    (should-error (cl-write-sequence (make-vector 3 0) s :start 99)
                  :type 'args-out-of-range)))

(ert-deftest cl-conf-write-sequence-returns-sequence ()
  (let ((v [72 105]))
    (should (equal (cl-conf--out (cl-write-sequence v s)) "Hi"))
    (should (equal v [72 105]))))

(ert-deftest cl-conf-write-sequence-string ()
  (should (equal (cl-conf--out (cl-write-sequence "Hi" s)) "Hi")))

(ert-deftest cl-conf-write-sequence-list ()
  (should (equal (cl-conf--out (cl-write-sequence (list ?H ?i) s)) "Hi")))

;; ── READ / READ-PRESERVING-WHITESPACE / READ-FROM-STRING ──────────────────

(ert-deftest cl-conf-read-forms ()
  (cl-conf--in "a 42 \"s\" (1 2)"
    (should (equal (cl-read s) 'a))
    (should (equal (cl-read s) 42))
    (should (equal (cl-read s) "s"))
    (should (equal (cl-read s) '(1 2)))))

(ert-deftest cl-conf-read-eof-value ()
  (cl-conf--in ""
    (should (eq (cl-read s nil :eof) :eof))
    (should-error (cl-read s) :type 'end-of-file)))

(ert-deftest cl-conf-read-symbol-at-eof ()
  ;; CL: a symbol/number immediately before EOF is read successfully.
  (cl-conf--in "sym" (should (equal (cl-read s) 'sym))))

(ert-deftest cl-conf-read-incomplete-signals ()
  ;; CL: signals regardless of eof-error-p when the object is incomplete.
  (cl-conf--in "("
    (should-error (cl-read s nil :eof) :type 'end-of-file)
    (should-error (cl-read s) :type 'end-of-file)))

(ert-deftest cl-conf-read-discards-delimiter ()
  "CL: read throws away the delimiting whitespace character."
  (cl-conf--in "42 x"
    (should (equal (cl-read s) 42))
    (should (equal (cl-read-char s) ?x))))

(ert-deftest cl-conf-read-preserving-whitespace-keeps-delimiter ()
  (cl-conf--in "42 x"
    (should (equal (cl-read-preserving-whitespace s) 42))
    (should (equal (cl-read-char s) ?\ ))))

(ert-deftest cl-conf-read-from-string-position ()
  (should (equal (car (cl-read-from-string "123")) 123))
  (should (equal (cdr (cl-read-from-string "123")) 3)))

(ert-deftest cl-conf-read-from-string-start ()
  "CLHS: (read-from-string \" 1 3 5\" t nil :start 2) => 3, 5."
  (should (equal (cl-read-from-string " 1 3 5" t nil :start 2) (cons 3 5))))

(ert-deftest cl-conf-read-from-string-delimiter ()
  (should (equal (cl-read-from-string "123 abc") (cons 123 4)))
  (should (equal (cl-read-from-string "123 abc" t nil :preserve-whitespace t)
                 (cons 123 3))))

(ert-deftest cl-conf-read-from-string-eof ()
  (should (eq (cl-read-from-string "" nil :eof) :eof))
  (should-error (cl-read-from-string "") :type 'end-of-file))

;; ── READ-DELIMITED-LIST ───────────────────────────────────────────────────

(ert-deftest cl-conf-read-delimited-list ()
  (cl-conf--in "1 2 3)"
    (should (equal (cl-read-delimited-list ?\) s) '(1 2 3)))))

(ert-deftest cl-conf-read-delimited-list-consumes-delimiter ()
  (cl-conf--in "1 2)x"
    (should (equal (cl-read-delimited-list ?\) s) '(1 2)))
    (should (equal (cl-read-char s) ?x))))

(ert-deftest cl-conf-read-delimited-list-eof-signals ()
  "CL: it is an error to reach end-of-file."
  (cl-conf--in "1 2 3"
    (should-error (cl-read-delimited-list ?\) s) :type 'end-of-file)))

;; ── large streams (regressions: the pushback ring used to overflow, and
;;    tokens split across a 4 KB read-chunk boundary used to be truncated) ──

(ert-deftest cl-conf-read-line-large-stream ()
  "cl-read-line must not lose lines once the stream exceeds one read chunk."
  (let* ((n 2000)
         (text (mapconcat (lambda (i) (format "line %d" i))
                          (number-sequence 1 n) "\n")))
    (cl-conf--in (concat text "\n")
      (let ((count 0) (last nil) line)
        (while (setq line (car (cl-read-line s nil nil)))
          (setq count (1+ count) last line))
        (should (= count n))
        (should (equal last (format "line %d" n)))))))

(ert-deftest cl-conf-read-many-forms-large-stream ()
  "cl-read must return every form of a large stream, in order."
  (let* ((n 3000)
         (text (mapconcat #'number-to-string (number-sequence 1 n) " ")))
    (cl-conf--in text
      (let ((i 0) (ok t))
        (while (< i n)
          (let ((v (cl-read s nil :eof)))
            (unless (eql v (1+ i)) (setq ok nil))
            (setq i (1+ i))))
        (should ok)
        (should (eq (cl-read s nil :eof) :eof))))))

(ert-deftest cl-conf-read-token-spans-chunk-boundary ()
  "A token longer than the 4 KB read chunk is read whole."
  (let ((sym (make-string 5000 ?a)))
    (cl-conf--in sym
      (should (equal (cl-read s) (intern sym))))))

(ert-deftest cl-conf-read-blank-at-eof ()
  "CL: blank lines / comments before EOF are not an incomplete object."
  (cl-conf--in "   "
    (should (eq (cl-read s nil :eof) :eof)))
  (cl-conf--in ";;; a comment"
    (should (eq (cl-read s nil :eof) :eof))))

(ert-deftest cl-conf-read-list-keeps-trailing-space ()
  "CL: a list's delimiter is `)', so the following whitespace is not eaten."
  (cl-conf--in "(a b) c"
    (should (equal (cl-read s) '(a b)))
    (should (equal (cl-read-char s) ?\s))))

(ert-deftest cl-conf-read-line-from-file ()
  "cl-read-line over a real file stream (feed via the fd window)."
  (cl-conf--tmp f
    (with-temp-file f
      (dotimes (i 1000) (insert (format "row %d\n" i))))
    (let ((s (cl-open f :direction :input)))
      (unwind-protect
          (let ((n 0) last line)
            (while (setq line (car (cl-read-line s nil nil)))
              (setq n (1+ n) last line))
            (should (= n 1000))
            (should (equal last "row 999")))
        (cl-close s)))))

(ert-deftest cl-conf-read-from-file ()
  "cl-read over a file stream reads every object (fd window)."
  (cl-conf--tmp f
    (with-temp-file f
      (dotimes (i 200) (insert (format "(%d . %d)\n" i (* i i)))))
    (let ((s (cl-open f :direction :input)))
      (unwind-protect
          (progn
            (dotimes (i 200)
              (should (equal (cl-read s) (cons i (* i i)))))
            (should (eq (cl-read s nil :eof) :eof)))
        (cl-close s)))))

(ert-deftest cl-conf-read-line-across-concatenated ()
  "cl-read-line spans the seam of a concatenated stream."
  (let ((a (cl-make-string-input-stream "one\ntwo"))
        (b (cl-make-string-input-stream "\nthree\n")))
    (let ((cc (cl-make-concatenated-stream a b)))
      (unwind-protect
          (progn
            (should (equal (car (cl-read-line cc)) "one"))
            (should (equal (car (cl-read-line cc)) "two"))
            (should (equal (car (cl-read-line cc)) "three"))
            (should (eq (car (cl-read-line cc nil :eof)) :eof)))
        (cl-close cc)))))

(ert-deftest cl-conf-read-line-from-buffer-stream ()
  "cl-read-line over a buffer-input stream (mem_cursor window)."
  (with-temp-buffer
    (insert "alpha\nbeta\ngamma")
    (let ((s (cl-make-buffer-input-stream (current-buffer))))
      (unwind-protect
          (progn
            (should (equal (car (cl-read-line s)) "alpha"))
            (should (equal (car (cl-read-line s)) "beta"))
            (should (equal (cl-read-line s) '("gamma" . t))))
        (cl-close s)))))

;; ── WRITE-CHAR / WRITE-STRING / WRITE-LINE / WRITE-BYTE ───────────────────

(ert-deftest cl-conf-write-char-returns-char ()
  (should (equal (cl-conf--out (cl-write-char ?A s)) "A")))

(ert-deftest cl-conf-write-string-returns-whole-string ()
  (let ((r (cl-conf--out (cl-write-string "abcdef" s :start 1 :end 4))))
    (should (equal r "bcd")))
  (should (equal (cl-write-string "abcdef" nil :start 1 :end 4) "abcdef")))

(ert-deftest cl-conf-write-line-appends-newline ()
  (should (equal (cl-conf--out (cl-write-line "ab" s)) "ab\n")))

(ert-deftest cl-conf-write-byte-returns-byte ()
  (let ((s (cl-make-string-output-stream)))
    (should (equal (cl-write-byte 65 s) 65))     ; returns the byte
    (should (equal (cl-get-output-stream-string s) "A"))
    (cl-close s)))

;; ── TERPRI / FRESH-LINE ───────────────────────────────────────────────────

(ert-deftest cl-conf-terpri ()
  (should (equal (cl-conf--out (cl-write-string "a" s) (cl-terpri s) (cl-write-string "b" s))
                 "a\nb")))

(ert-deftest cl-conf-fresh-line-at-bol-nil ()
  (let ((*standard-output* (cl-make-string-output-stream)))
    (should (null (cl-io-fresh-line)))
    (cl-close *standard-output*)))

(ert-deftest cl-conf-fresh-line-mid-line-writes-newline ()
  "CL: write a newline and return t when not at the start of a line."
  (let ((*standard-output* (cl-make-string-output-stream)))
    (cl-write-string "abc" *standard-output*)
    (should (cl-io-fresh-line))
    (should (equal (cl-get-output-stream-string *standard-output*) "abc\n"))
    (cl-close *standard-output*)))

;; ── FINISH/FORCE/CLEAR-OUTPUT, CLEAR-INPUT ────────────────────────────────

(ert-deftest cl-conf-output-control-return-nil ()
  (let ((s (cl-make-string-output-stream)))
    (cl-write-string "x" s)
    (should (null (cl-finish-output s)))
    (should (null (cl-force-output s)))
    (should (null (cl-clear-output s)))
    (should (equal (cl-get-output-stream-string s) ""))
    (cl-close s)))

(ert-deftest cl-conf-clear-input ()
  (cl-conf--in "data"
    (cl-unread-char (cl-read-char s) s)
    (should (null (cl-clear-input s)))
    (should-not (cl-listen s))))

;; ── PRINTER (write/prin1/princ/print/pprint/*-to-string) ──────────────────

(ert-deftest cl-conf-write-default-stream ()
  "CL: write's :stream defaults to *standard-output*."
  (let ((*standard-output* (cl-make-string-output-stream)))
    (cl-write "hi")
    (should (equal (cl-get-output-stream-string *standard-output*) "\"hi\""))
    (cl-close *standard-output*)))

(ert-deftest cl-conf-write-returns-object ()
  (let ((s (cl-make-string-output-stream)))
    (should (equal (cl-write 7 :stream s) 7))           ; returns the object
    (should (equal (cl-get-output-stream-string s) "7"))
    (cl-close s)))

(ert-deftest cl-conf-write-escape ()
  "CL: :escape controls whether escape characters are written."
  (should (equal (cl-write-to-string "hi" :escape t) "\"hi\""))
  (should (equal (cl-write-to-string "hi" :escape nil) "hi")))

(ert-deftest cl-conf-write-readably ()
  "CL: :readably is accepted (Emacs' printer never signals print-not-readable)."
  (should (equal (cl-write-to-string 42 :readably nil) "42"))
  (should (equal (cl-write-to-string "hi" :readably t) "\"hi\"")))

(ert-deftest cl-conf-write-print-controls ()
  (should (equal (cl-write-to-string (list 1 2 3 4) :length 2) "(1 2 ...)")))

(ert-deftest cl-conf-prin1-default-stream ()
  (let ((*standard-output* (cl-make-string-output-stream)))
    (should (equal (cl-io-prin1 "hi") "hi"))            ; returns object
    (should (equal (cl-get-output-stream-string *standard-output*) "\"hi\""))
    (cl-close *standard-output*)))

(ert-deftest cl-conf-princ-print-default-stream ()
  (let ((*standard-output* (cl-make-string-output-stream)))
    (should (equal (cl-princ "hi") "hi"))
    (should (equal (cl-get-output-stream-string *standard-output*) "hi"))
    (should (equal (cl-print 5) 5))
    (should (equal (cl-get-output-stream-string *standard-output*) "\n5 "))
    (cl-close *standard-output*)))

(ert-deftest cl-conf-pprint-no-values ()
  "CL: pprint returns no values (represented as nil)."
  (let ((s (cl-make-string-output-stream)))
    (should (null (cl-pprint 42 s)))
    (should (equal (cl-get-output-stream-string s) "42\n"))
    (cl-close s)))

(ert-deftest cl-conf-to-string ()
  (should (equal (cl-io-prin1-to-string "hi") "\"hi\""))
  (should (equal (cl-princ-to-string "hi") "hi"))
  (should (equal (cl-write-to-string '(1 2)) "(1 2)")))

;; ── OPEN / CLOSE ──────────────────────────────────────────────────────────

(ert-deftest cl-conf-open-input ()
  (cl-conf--tmp f
    (with-temp-file f (insert "hello"))
    (let ((s (cl-open f :direction :input)))
      (unwind-protect (should (equal (cl-read-line s) '("hello" . t)))
        (cl-close s)))))

(ert-deftest cl-conf-open-output-supersede ()
  (cl-conf--tmp f
    (with-temp-file f (insert "OLD"))
    (let ((s (cl-open f :direction :output :if-exists :supersede)))
      (cl-write-string "NEW" s) (cl-close s))
    (should (equal (with-temp-buffer (insert-file-contents f) (buffer-string)) "NEW"))))

(ert-deftest cl-conf-open-if-exists-nil ()
  (cl-conf--tmp f
    (with-temp-file f (insert "OLD"))
    (should-not (cl-open f :direction :output :if-exists nil))))

(ert-deftest cl-conf-open-if-does-not-exist-nil ()
  (should-not (cl-open "/nonexistent-emacs-io-conf" :direction :input
                       :if-does-not-exist nil)))

(ert-deftest cl-conf-open-if-does-not-exist-error ()
  (should-error (cl-open "/nonexistent-emacs-io-conf" :direction :input
                         :if-does-not-exist :error)
                :type 'file-error))

(ert-deftest cl-conf-open-default-if-exists-errors ()
  "CL: default :if-exists is :error."
  (cl-conf--tmp f
    (with-temp-file f (insert "OLD"))
    (should-error (cl-open f :direction :output) :type 'file-error)
    ;; the file must be left untouched
    (should (equal (with-temp-buffer (insert-file-contents f) (buffer-string)) "OLD"))))

(ert-deftest cl-conf-open-probe ()
  (cl-conf--tmp f
    (with-temp-file f (insert "x"))
    (let ((s (cl-open f :direction :probe)))
      (should s)
      (should-not (cl-open-stream-p s)))    ; probe stream is closed
    (should-not (cl-open "/nonexistent-emacs-io-conf" :direction :probe))))

(ert-deftest cl-conf-open-binary-element-type ()
  (cl-conf--tmp f
    (with-temp-file f (insert "AB"))
    (let ((s (cl-open f :direction :input :element-type '(unsigned-byte 8))))
      (unwind-protect
          (progn
            (should (equal (cl-stream-element-type s) '(unsigned-byte 8)))
            (should (equal (cl-read-byte s) 65)))
        (cl-close s)))))

(ert-deftest cl-conf-close-idempotent ()
  (let ((s (cl-make-string-input-stream "x")))
    (should (cl-close s))
    (should (cl-close s))))                 ; CL: implementation-dependent but ok

(ert-deftest cl-conf-close-does-not-close-constituents ()
  "CL: close of a constructed stream has no effect on its constituents."
  (let* ((in (cl-make-string-input-stream "abc"))
         (out (cl-make-string-output-stream))
         (tw (cl-make-two-way-stream in out)))
    (cl-close tw)
    (should (cl-open-stream-p in))
    (should (cl-open-stream-p out))))

;; ── STRING STREAMS ────────────────────────────────────────────────────────

(ert-deftest cl-conf-get-output-stream-string-clears ()
  "CL: get-output-stream-string clears the stream."
  (let ((s (cl-make-string-output-stream)))
    (cl-write-string "abc" s)
    (should (equal (cl-get-output-stream-string s) "abc"))
    (should (equal (cl-get-output-stream-string s) ""))
    (cl-close s)))

(ert-deftest cl-conf-make-string-input-start-end ()
  (let ((s (cl-make-string-input-stream "abcdef" 1 3)))
    (should (equal (cl-read-char s) ?b))
    (should (equal (cl-read-char s) ?c))
    (should (eq (cl-read-char s nil :eof) :eof))
    (cl-close s)))

;; ── CONSTRUCTED STREAMS ───────────────────────────────────────────────────

(ert-deftest cl-conf-synonym-stream ()
  (let ((*standard-input* (cl-make-string-input-stream "hi")))
    (let ((syn (cl-make-synonym-stream '*standard-input*)))
      (should (cl-input-stream-p syn))
      (should (equal (cl-read-char syn) ?h))
      (cl-close syn))))

(ert-deftest cl-conf-broadcast-stream ()
  (let ((a (cl-make-string-output-stream))
        (b (cl-make-string-output-stream)))
    (let ((bc (cl-make-broadcast-stream a b)))
      (cl-write-string "hi" bc)
      (cl-close bc))
    (should (equal (cl-get-output-stream-string a) "hi"))
    (should (equal (cl-get-output-stream-string b) "hi"))))

(ert-deftest cl-conf-concatenated-stream ()
  (let ((a (cl-make-string-input-stream "ab"))
        (b (cl-make-string-input-stream "cd")))
    (let ((cc (cl-make-concatenated-stream a b)))
      (should (equal (cl-read-char cc) ?a))
      (should (equal (cl-read-char cc) ?b))
      (should (equal (cl-read-char cc) ?c))
      (should (equal (cl-read-char cc) ?d))
      (cl-close cc))))

(ert-deftest cl-conf-two-way-stream ()
  (let ((in (cl-make-string-input-stream "hello"))
        (out (cl-make-string-output-stream)))
    (let ((tw (cl-make-two-way-stream in out)))
      (should (cl-input-stream-p tw))
      (should (cl-output-stream-p tw))
      (should (equal (cl-read-char tw) ?h))
      (cl-write-string "wo" tw)
      (cl-close tw))
    (should (equal (cl-get-output-stream-string out) "wo"))))

(ert-deftest cl-conf-echo-stream ()
  (let ((in (cl-make-string-input-stream "abc"))
        (out (cl-make-string-output-stream)))
    (let ((ec (cl-make-echo-stream in out)))
      (cl-read-char ec) (cl-read-char ec) (cl-read-char ec)
      (cl-close ec))
    (should (equal (cl-get-output-stream-string out) "abc"))))

(ert-deftest cl-conf-accessors ()
  ;; Accessors return a fresh user-ptr for the same underlying stream, so
  ;; compare behaviour rather than object identity.
  (let ((in (cl-make-string-input-stream "a"))
        (out (cl-make-string-output-stream)))
    (let ((tw (cl-make-two-way-stream in out)))
      (should (cl-input-stream-p (cl-two-way-stream-input-stream tw)))
      (should (cl-output-stream-p (cl-two-way-stream-output-stream tw)))
      (cl-close tw))
    (let ((ec (cl-make-echo-stream in out)))
      (should (cl-input-stream-p (cl-echo-stream-input-stream ec)))
      (should (cl-output-stream-p (cl-echo-stream-output-stream ec)))
      (cl-close ec))))

;; ── PREDICATES / INTROSPECTION ────────────────────────────────────────────

(ert-deftest cl-conf-streamp ()
  (should-not (cl-streamp nil))
  (should-not (cl-streamp 42))
  (should-not (cl-streamp "s"))
  (let ((s (cl-make-string-input-stream "x")))
    (should (cl-streamp s))
    (cl-close s)
    (should (cl-streamp s))))                ; still a stream when closed

(ert-deftest cl-conf-streamp-non-streams ()
  "CL: streamp is true only for streams."
  (should-not (cl-streamp (make-hash-table)))
  (should-not (cl-streamp [1 2]))
  (should-not (cl-streamp 'sym))
  (should-not (cl-streamp (current-buffer))))

(ert-deftest cl-conf-open-stream-p ()
  (let ((s (cl-make-string-input-stream "x")))
    (should (cl-open-stream-p s))
    (cl-close s)
    (should-not (cl-open-stream-p s))))

(ert-deftest cl-conf-input-output-stream-p ()
  (let ((in (cl-make-string-input-stream "x"))
        (out (cl-make-string-output-stream)))
    (should (cl-input-stream-p in))
    (should-not (cl-output-stream-p in))
    (should (cl-output-stream-p out))
    (should-not (cl-input-stream-p out))
    (cl-close in) (cl-close out)))

(ert-deftest cl-conf-stream-element-type ()
  (let ((s (cl-make-string-input-stream "x")))
    (should (eq (cl-stream-element-type s) 'character))
    (cl-close s)))

(ert-deftest cl-conf-interactive-stream-p ()
  (cl-conf--tmp f
    (with-temp-file f (insert "x"))
    (let ((s (cl-open f :direction :input)))
      (unwind-protect (should-not (cl-interactive-stream-p s))
        (cl-close s)))))

;; ── FILE-POSITION / FILE-LENGTH ───────────────────────────────────────────

(ert-deftest cl-conf-file-position-string-input ()
  (cl-conf--in "abcdef"
    (should (equal (cl-file-position s) 0))
    (cl-read-char s)
    (should (equal (cl-file-position s) 1))
    (should (cl-file-position s 3))
    (should (equal (cl-file-position s) 3))))

(ert-deftest cl-conf-file-position-inappropriate-signals ()
  "CL: an inappropriate position-spec signals an error."
  (cl-conf--in "abc"
    (should-error (cl-file-position s 99))
    (should-error (cl-file-position s -1))))

(ert-deftest cl-conf-file-position-start-end-designators ()
  "CL: a file position designator may be :start or :end."
  (cl-conf--in "abc"
    (cl-read-char s)
    (should (cl-file-position s :start))
    (should (equal (cl-read-char s) ?a))
    (should (cl-file-position s :end))
    (should (eq (cl-read-char s nil :eof) :eof))))

(ert-deftest cl-conf-file-length ()
  (cl-conf--tmp f
    (with-temp-file f (insert "abcdefgh"))
    (let ((s (cl-open f :direction :input)))
      (unwind-protect (should (equal (cl-file-length s) 8))
        (cl-close s)))))

;; ── documented gaps left for a product decision (REVIEW) ─────────────────

(ert-deftest cl-conf-synonym-stream-dynamic ()
  "CL: a synonym stream re-resolves its symbol on each use."
  (let ((*standard-input* (cl-make-string-input-stream "a")))
    (let ((syn (cl-make-synonym-stream '*standard-input*)))
      (set '*standard-input* (cl-make-string-input-stream "b"))
      (should (equal (cl-read-char syn) ?b))
      (cl-close syn))))

(ert-deftest cl-conf-close-abort-removes-created-file ()
  "CL: :abort deletes a file created for an output stream."
  (let ((f (make-temp-name "/tmp/cl-conf-abort-")))
    (unwind-protect
        (progn
          (let ((s (cl-open f :direction :output :if-does-not-exist :create)))
            (cl-write-string "x" s)
            (cl-close s :abort t))
          (should-not (file-exists-p f)))
      (ignore-errors (delete-file f)))))

(ert-deftest cl-conf-concatenated-stream-streams-remaining ()
  "CL: concatenated-stream-streams returns the streams not yet exhausted."
  (let ((a (cl-make-string-input-stream "ab"))
        (b (cl-make-string-input-stream "cd")))
    (let ((cc (cl-make-concatenated-stream a b)))
      (should (equal (length (cl-concatenated-stream-streams cc)) 2))
      (cl-read-char cc) (cl-read-char cc) (cl-read-char cc) ; a, b, then advance->c
      (should (equal (length (cl-concatenated-stream-streams cc)) 1))
      (should (equal (cl-read-char cc) ?d))
      (should (eq (cl-read-char cc nil :eof) :eof))         ; forces advance past end
      (should-not (cl-concatenated-stream-streams cc))
      (cl-close cc))))

(ert-deftest cl-conf-write-char-above-255 ()
  "CL: write-char writes the character (UTF-8 encoded)."
  (let ((s (cl-make-string-output-stream)))
    (cl-write-char 955 s)                 ; λ
    (should (equal (cl-get-output-stream-string s) "λ"))
    (cl-close s)))

;; ── additional coverage ───────────────────────────────────────────────────

(ert-deftest cl-conf-read-chunk ()
  "cl-read-chunk returns (CHUNK . EOF-P) and advances the cursor."
  (cl-conf--in "hello world"
    (let ((a (cl-read-chunk s 5)))
      (should (equal (car a) "hello"))
      (should-not (cdr a)))              ; more data remains
    (let ((b (cl-read-chunk s 100)))
      (should (equal (car b) " world"))
      (should (cdr b)))                  ; reached end of file
    (let ((c (cl-read-chunk s 5)))
      (should (equal (car c) ""))
      (should (cdr c)))))

(ert-deftest cl-conf-file-string-length ()
  "cl-file-string-length is the byte count of the printed object."
  (let ((s (cl-make-string-output-stream)))
    (should (equal (cl-file-string-length s "abc") 5))   ; "\"abc\""
    (should (equal (cl-file-string-length s 42) 2))
    (cl-close s)))

(ert-deftest cl-conf-broadcast-stream-streams ()
  "cl-broadcast-stream-streams returns the constituent streams."
  (let ((a (cl-make-string-output-stream))
        (b (cl-make-string-output-stream)))
    (let ((bc (cl-make-broadcast-stream a b)))
      (should (equal (length (cl-broadcast-stream-streams bc)) 2))
      (cl-close bc))
    (cl-close a) (cl-close b)))

(ert-deftest cl-conf-file-stream-write ()
  "Writing (with :start/:end, and cl-write) to a file stream."
  (cl-conf--tmp f
    (let ((s (cl-open f :direction :output :if-exists :supersede)))
      (cl-write-string "abc" s :start 1 :end 2)   ; "b"
      (cl-write "x" :stream s)                    ; "\"x\""
      (cl-close s))
    (should (equal (with-temp-buffer (insert-file-contents f) (buffer-string))
                   "b\"x\""))))

(ert-deftest cl-conf-write-keywords ()
  "cl-write accepts every CL print control; unsupported ones are ignored."
  (should (equal (cl-write-to-string "hi" :escape nil) "hi"))
  ;; these have no Emacs variable, so they are accepted and ignored
  (should (equal (cl-write-to-string 255 :base 16) "255"))
  (should (equal (cl-write-to-string 'abc :case :downcase) "abc"))
  (should (equal (cl-write-to-string '(1 2) :array t) "(1 2)"))
  ;; :pretty routes through Emacs' pretty-printer
  (should (stringp (cl-write-to-string '(1 (2 3)) :pretty t))))

(ert-deftest cl-conf-unread-char-strict ()
  "CL: unread must name the last read char and may not repeat."
  (cl-conf--in "xy"
    ;; nothing read yet -> nothing to unread
    (should-error (cl-unread-char ?z s))
    (should (equal (cl-read-char s) ?x))
    ;; must be the most recently read character
    (should-error (cl-unread-char ?y s))
    ;; unreading the most recent character once is fine
    (cl-unread-char ?x s)
    ;; but not twice without an intervening read
    (should-error (cl-unread-char ?x s))
    (should (equal (cl-read-char s) ?x))
    (should (equal (cl-read-char s) ?y))
    ;; after a new read, the old character is no longer acceptable
    (should-error (cl-unread-char ?x s))
    (cl-unread-char ?y s)
    (should (equal (cl-read-char s) ?y))))

(ert-deftest cl-conf-ftruncate-non-file-signals ()
  "Truncation requires a real file; anything else is an error."
  (let ((s (cl-make-string-output-stream)))
    (should-error (cl-ftruncate s 0))
    (cl-close s)))

(ert-deftest cl-conf-printer-names-distinct-from-emacs ()
  "We yield `cl-prin1'/`cl-prin1-to-string'/`cl-fresh-line' to Emacs' cl-lib
and expose stream-aware versions as `cl-io-prin1'/`cl-io-fresh-line' instead."
  (require 'cl-print)
  (require 'cl-extra)
  ;; Emacs' functions are untouched, ours are separate objects...
  (should-not (eq (symbol-function 'cl-io-prin1) (symbol-function 'cl-prin1)))
  (should-not (eq (symbol-function 'cl-io-fresh-line)
                  (symbol-function 'cl-fresh-line)))
  ;; ...and ours work on our streams.
  (let ((s (cl-make-string-output-stream)))
    (should (equal (cl-io-prin1 "hi" s) "hi"))
    (should (equal (cl-get-output-stream-string s) "\"hi\""))
    (cl-close s))
  (let ((s (cl-make-string-output-stream)))
    (cl-write-string "abc" s)
    (should (cl-io-fresh-line s))
    (should (equal (cl-get-output-stream-string s) "abc\n"))
    (cl-close s)))

(provide 'emacs-io-conformance-test)
;;; emacs-io-conformance-test.el ends here
