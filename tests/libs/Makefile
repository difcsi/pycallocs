LIBALLOCS ?= ../../contrib/liballocs
CC = $(LIBALLOCS)/tools/lang/c/bin/allocscc

all: $(patsubst %.c,%.so,$(filter-out $(wildcard *.cil.*) $(wildcard *.allocstubs.*),$(wildcard *.c)))

%.so: %.c
	$(CC) -std=gnu11 -g -fPIC -shared -I$(LIBALLOCS)/include $< -o $@

clean:
	rm -f *.o *.cil.* *.i *.fixuplog *.allocs *.allocstubs.* *.so

.PHONY: all clean 
