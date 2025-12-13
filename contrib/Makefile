THIS_MAKEFILE := $(realpath $(lastword $(MAKEFILE_LIST)))
CONTRIB := $(realpath $(dir $(THIS_MAKEFILE)))
default: $(CONTRIB)/env.sh config.mk build-liballocs build-linkpy build-alaska build-cpython


include config.mk



# ---------------------------- alaska

CONFIG_MK += \nALASKA := $(CONTRIB)/alaska\n
varlist += ALASKA

.PHONY: build-alaska-deps
build-alaska-deps:
	$(MAKE) -C alaska deps

.PHONY: build-alaska
build-alaska: build-alaska-deps
	${MAKE} -C alaska

AENV:= CC=$(CONTRIB)/alaska/local/bin/alaska CXX=$(CONTRIB)/alaska/local/bin/alaska++ LD_LIBRARY_PATH=${CONTRIB}/alaska/local/lib:${LD_LIBRARY_PATH} PATH=${CONTRIB}/alaska/local/bin:${PATH}
#CC := ${CONTRIB}/alaska/local/bin/alaska
#CXX := ${CONTRIB}/alaska/local/bin/alaska++
LD_LIBRARY_PATH := ${CONTRIB}/alaska/local/lib:${LD_LIBRARY_PATH}
#PATH := ${CONTRIB}/alaska/local/bin:${PATH}

#CONFIG_MK += \nCC := ${CONTRIB}/alaska/local/bin/alaska\n
#CONFIG_MK += \nCXX := ${CONTRIB}/alaska/local/bin/alaska++\n
CONFIG_MK += \nLD_LIBRARY_PATH := ${CONTRIB}/alaska/local/lib:${LD_LIBRARY_PATH}\n
#CONFIG_MK += \nPATH := ${CONTRIB}/alaska/local/bin:${PATH}\n
CONFIG_MK += \nAENV := ${AENV}\n

varlist+=AENV
#varlist+=CC
#varlist+=CXX
varlist+=LD_LIBRARY_PATH
#varlist+=PATH


# ---------------------------- cpython
CONFIG_MK += \nPYTHON := $(CONTRIB)/cpython/python\n
PYTHON := $(CONTRIB)/cpython/python
varlist += PYTHON

cpython/Makefile:
	cd cpython && ./configure --prefix=$(CONTRIB)/cpython/local

.PHONY: build-cpython
build-cpython: build-alaska cpython/Makefile
	$(MAKE) -C cpython
	$(PYTHON) -m ensurepip --upgrade
	$(PYTHON) -m pip install setuptools




# ---------------------------- liballocs
 
CONFIG_MK += \nLIBALLOCS := $(CONTRIB)/liballocs\n
varlist += LIBALLOCS
subconfigs += liballocs/contrib/config.mk

liballocs/contrib/config.mk:
	$(MAKE) -C liballocs/contrib


liballocs/configure: liballocs/contrib/config.mk
	. liballocs/contrib/env.sh && \
	cd liballocs && ./autogen.sh && ./configure --enable-lifetime-policies
	
.PHONY: build-liballocs
build-liballocs: liballocs/configure
	$(MAKE) -C liballocs

# ---------------------------- linkpy
CONFIG_MK += \nLINKPY := $(CONTRIB)/linkpy\n
varlist += LINKPY

.PHONY: build-linkpy
build-linkpy: build-cpython
	PYTHON=$(PYTHON) $(MAKE) -C linkpy

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
	$(MAKE) -C liballocs clean
	$(MAKE) -C liballocs/contrib clean
	$(MAKE) -C alaska clean
	$(MAKE) -C cpython clean
	$(MAKE) -C linkpy clean
	rm -f config.mk env.sh
.PHONY: clean
clean:
	rm -f config.mk env.sh