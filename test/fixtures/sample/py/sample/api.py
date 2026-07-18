class Ops:
    @staticmethod
    def add(a, b):
        return _lib.core_add(a, b)


def add(a, b):
    return Ops.add(a, b)
