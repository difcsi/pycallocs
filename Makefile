PYTHON = python3

all: build install

build:
	$(PYTHON) setup.py build

tests: install
	make -C tests

install: build venv
	. venv/bin/activate && $(PYTHON) setup.py install

venv:
	virtualenv -p $(PYTHON) venv

clean:
	rm -rf venv build *.pyc

.PHONY: all build clean install tests
