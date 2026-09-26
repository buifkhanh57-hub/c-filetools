CC       ?= cc
CFLAGS   ?= -O2
STD       = -std=c11
WARN      = -Wall -Wextra
# -std=c11 is strict ISO C; diskdive deliberately uses the POSIX.1-2008
# API surface (lstat, opendir, realpath, localtime_r, ...), so the
# feature-test macro is part of the build contract, not optional.
CPPFLAGS += -D_XOPEN_SOURCE=700 -Isrc

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)
BIN = diskdive

.PHONY: all test clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c
	$(CC) $(STD) $(WARN) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	./tests/run_tests.sh

clean:
	rm -f $(OBJ) $(BIN)
