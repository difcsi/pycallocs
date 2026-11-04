import sys


def relative_src_on_path():
    return int('src' in sys.path)
