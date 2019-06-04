CC = $(LIBALLOCS)/tools/lang/c/bin/allocscc

all: $(patsubst %.c,%.so,$(filter-out $(wildcard *.cil.*) $(wildcard *.allocstubs.*),$(wildcard *.c)))

%.so: %.c
	$(CC) -g -fPIC -shared $< -o $@

clean:
	rm -f *.o *.cil.* *.i *.fixuplog *.allocs *.allocstubs.* *.so

.PHONY: all clean 
