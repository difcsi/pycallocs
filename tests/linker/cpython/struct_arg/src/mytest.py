class HelloWorld:
    def __init__(self, a):
        self.a = a


def describe(hw) -> int:
    # hw arrives from C, marshalled by T_to_PyObject, carrying the struct's fields.
    print("Python got HelloWorld with a =", hw.a)
    return hw.a * 2
