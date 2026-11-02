import threading
import time

_n = 0


def _worker():
    global _n
    for _ in range(50):
        _n += 1
        time.sleep(0.005)


def start():
    threading.Thread(target=_worker, daemon=True).start()


def count():
    return _n
