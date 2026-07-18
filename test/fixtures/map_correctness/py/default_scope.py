def dependency():
    return object


def fetch(value=dependency()):
    return value


class Box(dependency()):
    pass
