THIS_MAKEFILE := $(realpath $(lastword $(MAKEFILE_LIST)))
CONTRIB := $(realpath $(dir $(THIS_MAKEFILE)))


# build liballocs 

build-liballocs:
	${MAKE} -C ${CONTRIB}/liballocs/contrib
	${CONTRIB}/liballocs/autogen.sh && \
	(. ${CONTRIB}/liballocs/contrib/env.sh && ${CONTRIB}/liballocs/configure) && \
	make -C  ${CONTRIB}/liballocs