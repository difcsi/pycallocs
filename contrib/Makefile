THIS_MAKEFILE := $(realpath $(lastword $(MAKEFILE_LIST)))
CONTRIB := $(realpath $(dir $(THIS_MAKEFILE)))
default: $(CONTRIB)/env.sh config.mk build-linkpy build-cpython


include config.mk


# ---------------------------- stackscan-owned dependencies
# liballocs and alaska are submodules of stackscan, not of pycallocs. They are
# built within stackscan; here we only export their locations so that the test
# Makefiles and setup.py can find the pre-built artifacts.
STACKSCAN := $(CONTRIB)/stackscan

CONFIG_MK += \nLIBALLOCS := $(STACKSCAN)/contrib/liballocs\n
varlist += LIBALLOCS

CONFIG_MK += \nALASKA := $(STACKSCAN)/contrib/alaska\n
varlist += ALASKA


# ---------------------------- cpython
CONFIG_MK += \nPYTHON := $(CONTRIB)/cpython/python\n
PYTHON := $(CONTRIB)/cpython/python
varlist += PYTHON

cpython/Makefile:
	cd cpython && ./configure --prefix=$(CONTRIB)/cpython/local CFLAGS=-fPIC


.PHONY: build-cpython
build-cpython: cpython/Makefile
	$(MAKE) -C cpython
	$(PYTHON) -m ensurepip --upgrade
	$(PYTHON) -m pip install setuptools


# ---------------------------- linkpy
CONFIG_MK += \nLINKPY := $(CONTRIB)/linkpy\n
varlist += LINKPY

.PHONY: build-linkpy
build-linkpy: build-cpython
	cmake -S linkpy -B linkpy/build -DPYTHON=$(PYTHON)
	cmake --build linkpy/build -j$(shell nproc)

# ----------------------------config.mk
# If the user nukes a sub-config.mk that our config.mk depends on,
# we'd like to be able to rebuild it. So we include a generic rule
# for rebuilding config.mks.
config.mk: $(subconfigs)
	(printf '%%/config.mk:\n\t$$(MAKE) -C $$* config.mk\n\n'; \
	printf "$(foreach cfg,$(subconfigs),\ninclude $(CONTRIB)/$(cfg))"; \
	printf '$(CONFIG_MK)'; echo) > "$@" || (rm -f "$@"; false)
# env.sh exports some vars
# We put $(CONTRIB)/env.sh and env.sh in the same target because $(CONTRIB) is always realpath'd
# and so when the generic env.sh rule tries to build a fully-qualified env.sh, it matches.
env.sh $(CONTRIB)/env.sh: $(foreach cfg,$(subconfigs),$(dir $(CONTRIB)/$(cfg))env.sh)
	(printf "$(foreach cfg,$(subconfigs),\n. $$(dirname $(CONTRIB)/$(cfg))/env.sh)"; \
	printf '$(foreach var,$(varlist),\nexport $(var)="$($(var))")'; echo )>"$@" || (rm -f "$@"; false)
# Similarly when we make an env.sh, have Make ensure existence of
# its depended-on env.sh too, so that we don't get 'source'-time
# failures of sub-env.sh.
$(foreach cfg,$(subconfigs),$(dir $(CONTRIB)/$(cfg))env.sh): %/env.sh:
	$(MAKE) -C $* env.sh

.PHONY: clean-all
clean-all: clean
	$(MAKE) -C cpython clean
	rm -rf linkpy/build linkpy/dist
	rm -f config.mk env.sh
.PHONY: clean
clean:
	rm -f config.mk env.sh
