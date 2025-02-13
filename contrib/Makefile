SUBDIRS = liballocs

default: build-liballocs

.PHONY: build-liballocs
build-liballocs:
	cd liballocs && \
	${MAKE} -C contrib && \
	. contrib/env.sh && \
	./autogen.sh && \
	./configure --enable-lifetime-policies && \
	$(MAKE)

clean:
	cd liballocs && $(MAKE) clean