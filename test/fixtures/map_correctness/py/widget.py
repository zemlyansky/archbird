class Widget:
    @staticmethod
    def build():
        return "built"


class Receiver:
    @classmethod
    def create(cls):
        return cls()

    def chain(self):
        return self

    def run(self):
        return "ran"


class Wrapper:
    def __init__(self, function):
        self.function = function

    def __call__(self):
        return self.function()
