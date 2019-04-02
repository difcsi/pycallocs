PYTHON = python3

all: build install

build: allocsmodule.c setup.py
	$(PYTHON) setup.py build

install: build venv
	. venv/bin/activate && $(PYTHON) setup.py install

venv:
	virtualenv -p $(PYTHON) venv

clean:
	rm -rf venv build

.PHONY: all clean install
