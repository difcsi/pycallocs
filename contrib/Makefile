THIS_MAKEFILE := $(realpath $(lastword $(MAKEFILE_LIST)))
CONTRIB := $(realpath $(dir $(THIS_MAKEFILE)))
default: $(CONTRIB)/env.sh config.mk build-liballocs


include config.mk



# ---------------------------- liballocs
 
CONFIG_MK += \nLIBALLOCS := $(CONTRIB)/liballocs\n
varlist += LIBALLOCS
subconfigs += liballocs/contrib/config.mk


.PHONY: build-liballocs-deps
build-liballocs-deps:
	$(MAKE) -C liballocs/contrib

.PHONY: configure-liballocs
configure-liballocs: build-liballocs-deps
	. liballocs/contrib/env.sh && \
	cd liballocs && ./autogen.sh && ./configure --enable-lifetime-policies
	
.PHONY: build-liballocs
build-liballocs: configure-liballocs
	$(MAKE) -C liballocs



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
	rm -f config.mk env.sh
.PHONY: clean
clean:
	rm -f config.mk env.sh