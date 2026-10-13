CC       ?= gcc
CFLAGS   ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -fPIC
LDFLAGS  ?= -shared
LDLIBS   ?= -lpthread
EMACS    ?= emacs

SRC      = src
TST      = test
TARGET   = emacs-io.so
OBJS     = $(SRC)/emacs-io.o $(SRC)/util.o $(SRC)/stream.o $(SRC)/fdstream.o \
           $(SRC)/strstream.o $(SRC)/mmapstream.o $(SRC)/bufstream.o \
           $(SRC)/stdstream.o $(SRC)/reader.o $(SRC)/printer.o \
           $(SRC)/fileio.o $(SRC)/lispio.o $(SRC)/compstream.o
TEST     = $(TST)/emacs-io-test.el
CONF     = $(TST)/emacs-io-conformance-test.el
BENCH    = bench/emacs-io-perf-test.el

.PHONY: all clean check verify test bench

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(SRC)/%.o: $(SRC)/%.c $(SRC)/stream.h $(SRC)/util.h $(SRC)/subrs.h Makefile
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS) $(TEST:.el=.elc) $(CONF:.el=.elc) $(BENCH:.el=.elc)

check: $(TARGET)
	@echo "Module:   $(TARGET)"
	@file $(TARGET)
	@echo "Exports:  $$(nm -D $(TARGET) | grep ' T ' | grep -E '^[0-9a-f]+ T (F|cl_|e)' | wc -l) symbols"
	@echo ""
	@echo "Targets:"
	@echo "  make test     — unit tests + Common Lisp conformance tests"
	@echo "  make bench    — performance / scale tests"

verify: $(TARGET)
	@$(EMACS) --batch -Q --eval '(module-load "$(CURDIR)/$(TARGET)")' \
		--eval '(message "emacs-io %d functions loaded" (length (apropos-internal "^cl-" (function fboundp))))'

test: $(TARGET)
	@rm -f $(TEST:.el=.elc) $(CONF:.el=.elc)
	@echo "=== unit tests ==="
	@$(EMACS) --batch -Q -L $(SRC) -L $(TST) -l $(TEST) -l $(CONF) -f ert-run-tests-batch-and-exit

bench: $(TARGET)
	@rm -f $(BENCH:.el=.elc)
	@echo "=== benchmarks ==="
	@$(EMACS) --batch -Q -L $(SRC) -L bench -l $(BENCH) -f ert-run-tests-batch-and-exit
