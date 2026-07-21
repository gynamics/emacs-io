CC       = gcc
CFLAGS   = -std=c11 -Wall -Wextra -O2 -fPIC
LDFLAGS  = -shared
LDLIBS   = -lpthread
EMACS    = emacs

TARGET   = emacs-io.so
OBJS     = emacs-io.o util.o stream.o fdstream.o strstream.o \
           stdstream.o reader.o printer.o fileio.o lispio.o
TEST     = emacs-io-test.el

.PHONY: all clean check test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c stream.h util.h Makefile
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS) $(TEST:.el=.elc)

check: $(TARGET)
	@echo "Module built: $(TARGET)"
	@file $(TARGET)

test: $(TARGET)
	@rm -f $(TEST:.el=.elc)
	$(EMACS) --batch -Q -L . -l $(TEST) -f ert-run-tests-batch-and-exit
