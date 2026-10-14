CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic

rvasm: src/rvasm.c
	$(CC) $(CFLAGS) -o $@ $<

test: rvasm
	./tests/run.sh

clean:
	rm -f rvasm

.PHONY: test clean
