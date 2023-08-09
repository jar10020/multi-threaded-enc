CC=gcc
CFLAGS= -O3 -g -pedantic -std=gnu17 -Wall -Werror -Wextra -pthread
LDFLAGS=-pthread

.PHONY: all
all: nyuenc

nyuenc: nyuenc.o 

nyuenc.o: nyuenc.c


.PHONY: clean
clean:
	rm -f *.o nyuenc
