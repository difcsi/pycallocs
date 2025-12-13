CC = $(LIBALLOCS)/tools/lang/c/bin/allocscc

C_SOURCES := $(filter-out $(wildcard *.cil.*) $(wildcard *.allocstubs.*) $(wildcard *.usedtypes.*),$(wildcard *.c))
SO_TARGETS := $(patsubst %.c,%.so,$(C_SOURCES))

all: $(SO_TARGETS)

%.so: %.c
	$(CC) -std=gnu11 -g -fPIC -shared -I$(LIBALLOCS)/include $< -o $@

clean:
	rm -f *.o *.cil.* *.i *.fixuplog *.allocs *.allocstubs.* *.so *.usedtypes.*

.PHONY: all clean 