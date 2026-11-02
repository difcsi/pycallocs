# Same module as simple_divide; the difference is in main.c, which resolves
# divide() at runtime via dlopen + dlsym rather than linking it at load time.
def divide(a, b):
    return a / b
