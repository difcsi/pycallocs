ACLOCAL_AMFLAGS = -I m4

# Python extension module sources
PYTHON_SOURCES = \
	allocs_module.c \
	library_loader.c \
	proxy.c \
	foreign_type.c \
	foreign_basetype.c \
	function_proxy.c \
	composite_proxy.c \
	address_proxy.c

PYTHON_MODULES = elflib.py

# Compiler and flags configuration
# Use allocscc for compilation to generate uniqtype symbols
CC = @allocscc@
export CC

# Include paths
AM_CPPFLAGS = \
	-I@liballocs@/include \
	-I@librunt@/include \
	-I@liballocstool@/include \
	-I$(srcdir)/include \
	-I@PYTHON_INCLUDE@

# Compile flags
AM_CFLAGS = \
	-Wall \
	-std=gnu99 \
	-ggdb \
	-gdwarf-4 \
	-fPIC \
	-DLIFETIME_POLICIES \
	-D_Float64=double \
	-D_Float128=long\ double \
	-D_Float64x=long\ double \
	-D__float128=long\ double \
	-Dnullptr=NULL \
	-Dtrue=1 \
	-Dfalse=0

if DEBUG
AM_CFLAGS += -O0
else
AM_CFLAGS += -O2
endif

# Library paths
AM_LDFLAGS = \
	-L@liballocs@/lib \
	-Wl,-rpath,@liballocs@/lib

# Libraries to link
LIBS = -ldl -lffi -lallocs

# Build targets
all-local: build-python

build-python: $(PYTHON_SOURCES) $(PYTHON_MODULES)
	@echo "Building Python extension module..."
	$(PYTHON) setup.py build

install-exec-local: build-python
	@echo "Installing Python extension module..."
	$(PYTHON) setup.py install --prefix=$(prefix)

clean-local:
	rm -rf build *.pyc __pycache__ *.egg-info
	find . -name "*.pyc" -delete
	find . -name "__pycache__" -type d -delete

check-local: install-exec-local
	$(MAKE) -C tests

.PHONY: build-python install-python clean-python check-python

# Distribution files
EXTRA_DIST = \
	$(PYTHON_SOURCES) \
	$(PYTHON_MODULES) \
	setup.py \
	requirements.txt \
	README.md \
	include/foreign_library.h \
	include/minicrunch.h \
	tests

# Additional headers
noinst_HEADERS = \
	include/foreign_library.h \
	include/minicrunch.h
