THIS_MAKEFILE := $(realpath $(lastword $(MAKEFILE_LIST)))
CONTRIB := $(realpath $(dir $(THIS_MAKEFILE)))


# things we need to buil d:
# - liballocs


# build liballocs 

build-liballocs:
	${MAKE} -C ${CONTRIB}/liballocs/contrib
	${CONTRIB}/liballocs/autogen.sh && \
	(. ${CONTRIB}/liballocs/contrib/env.sh && ${CONTRIB}/liballocs/configure --enable-lifetime-policies) && \
	${MAKE} -C  ${CONTRIB}/liballocs



# ----------------------------config.mk
# If the user nukes a sub-config.mk that our config.mk depends on,
# we'd like to be able to rebuild it. So we include a generic rule
# for rebuilding config.mks.
config.mk: $(subconfigs)
	(printf '%%/config.mk:\n\t$$(MAKE) -C $$* config.mk\n\n'; \
	printf "$(foreach cfg,$(subconfigs),\ninclude $(CONTRIB)/$(cfg))"; \
	printf '$(CONFIG_MK)'; echo) > "$@" || (rm -f "$@"; false)

# Similarly when we make an env.sh, have Make ensure existence of
# its depended-on env.sh too, so that we don't get 'source'-time
# failures of sub-env.sh.
$(foreach cfg,$(subconfigs),$(dir $(CONTRIB)/$(cfg))env.sh): %/env.sh:
	$(MAKE) -C $* env.sh
clean::
	rm -f config.mk env.sh
