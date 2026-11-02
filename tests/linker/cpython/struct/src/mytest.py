

class HelloWorld:
    a = 1
    def __init__(self, a):
        self.a = a

def phelloworld2() -> HelloWorld:
    print("Hello World from python")
    return HelloWorld(13)