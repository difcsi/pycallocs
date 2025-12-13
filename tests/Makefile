export PYTHONPATH := $(abspath $(CURDIR)/..)${PYTHONPATH:+:$(PYTHONPATH)}

%.py: libs
	@printf "Testing \033[1m$*\033[0m: "
	@if LD_PRELOAD=${LIBALLOCS}/lib/liballocs_preload.so $(PYTHON) $@ > $*.tmp 2>&1; then \
		if [ -f $*.out ] && ! diff -q $*.tmp $*.out > /dev/null 2>&1; then \
			echo -e "\033[1;31mFailed (output differs)\033[0m"; \
		else \
			echo -e "\033[1;32mSuccess\033[0m"; \
		fi; \
	else \
		echo -e "\033[1;35mFailed\033[0m"; \
		cat $*.tmp; \
	fi
	@rm -f $*.tmp

libs:
	$(MAKE) -C libs

clean:
	$(MAKE) -C libs clean
	rm -f *.tmp

all:  $(wildcard *.py)
	

.PHONY: libs clean
