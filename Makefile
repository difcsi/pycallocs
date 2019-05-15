PYTHON = python3

all: build install

build:
	$(PYTHON) setup.py build

tests: install
	make -C tests

install: build venv
	. venv/bin/activate && python setup.py install

venv:
	$(PYTHON) -m venv venv

clean:
	rm -rf venv build *.pyc

.PHONY: all build clean install tests
