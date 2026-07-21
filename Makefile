CC       ?= gcc
CFLAGS   ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -fPIC
LDFLAGS  ?= -shared
LDLIBS   ?= -lpthread
EMACS    ?= emacs

TARGET   = emacs-io.so
OBJS     = emacs-io.o util.o stream.o fdstream.o strstream.o \
           stdstream.o reader.o printer.o fileio.o lispio.o
TEST     = emacs-io-test.el
BENCH    = emacs-io-perf-test.el
BENCH_LOOPS ?= 1

.PHONY: all clean check verify test bench

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c stream.h util.h Makefile
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS) $(TEST:.el=.elc) $(BENCH:.el=.elc)

check: $(TARGET)
	@echo "Module:   $(TARGET)"
	@file $(TARGET)
	@echo "Exports:  $$(nm -D $(TARGET) | grep ' T ' | grep -E '^[0-9a-f]+ T (F|cl_|e)' | wc -l) symbols"

verify: $(TARGET)
	@$(EMACS) --batch -Q --eval '(module-load "$(CURDIR)/$(TARGET)")' \
		--eval '(message "emacs-io %d functions loaded" (length (apropos-internal "^cl-" (function fboundp))))'

test: $(TARGET)
	@rm -f $(TEST:.el=.elc)
	@echo "=== unit tests ==="
	@$(EMACS) --batch -Q -L . -l $(TEST) -f ert-run-tests-batch-and-exit

bench: $(TARGET)
	@rm -f $(BENCH:.el=.elc)
	@echo "=== benchmarks ==="
	@$(EMACS) --batch -Q -L . -l $(BENCH) -f ert-run-tests-batch-and-exit
