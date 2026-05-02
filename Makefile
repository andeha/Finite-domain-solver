CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2 -g

OBJS = trail.o csp.o

all: examples regalloc

trail.o: trail.c trail.h
	$(CC) $(CFLAGS) -c -o $@ $<

csp.o: csp.c csp.h trail.h
	$(CC) $(CFLAGS) -c -o $@ $<

examples: examples.c $(OBJS) csp.h
	$(CC) $(CFLAGS) -o $@ examples.c $(OBJS)

regalloc: regalloc.c $(OBJS) csp.h
	$(CC) $(CFLAGS) -o $@ regalloc.c $(OBJS)

run: examples
	./examples

clean:
	rm -f *.o examples dwta inspector regalloc

.PHONY: all clean run
