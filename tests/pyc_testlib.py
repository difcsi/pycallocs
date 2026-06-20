"""Shared helper for the pycallocs test suite.

Deduplicates the boilerplate that every test used to carry for locating the
compiled fixture libraries and adding them to elflib's import search path.

The fixture .so files are built into the build tree and located at run time via
the PYCALLOCS_TEST_LIBS environment variable (set by tests/CMakeLists.txt). When
that variable is absent -- e.g. a test run by hand from a checkout -- we fall
back to the in-source libs/ directory next to this file.
"""
import os
import elflib

_LIBS = os.environ.get(
    "PYCALLOCS_TEST_LIBS",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "libs"),
)


def add_libs(*names):
    """Make each named fixture library importable via ``from elflib import <name>``.

    Each lib lives in its own subfolder (``<libs>/<name>/<name>.so``), so we add
    that subfolder to elflib's search path.
    """
    for name in names:
        path = os.path.join(_LIBS, name)
        if path not in elflib.__path__:
            elflib.__path__.append(path)
