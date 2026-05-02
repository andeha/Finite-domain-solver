CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2 -g

OBJS = trail.o csp.o

all: examples

trail.o: trail.c trail.h
	$(CC) $(CFLAGS) -c -o $@ $<

csp.o: csp.c csp.h trail.h
	$(CC) $(CFLAGS) -c -o $@ $<

examples: examples.c $(OBJS) csp.h
	$(CC) $(CFLAGS) -o $@ examples.c $(OBJS)

run: examples
	./examples

clean:
	rm -f *.o examples

.PHONY: all clean run
