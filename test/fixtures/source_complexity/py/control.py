def py_flow(a, b):
    if a and b:
        for value in range(a):
            if value == b:
                return value
    elif a or b:
        result = 1 if a else 0
    match a:
        case 0:
            result = 0
        case _:
            result = 1
    try:
        with open("value.txt") as stream:
            result += len(stream.read())
    except OSError:
        result = 0
    assert result >= 0
    return result


def py_outer(callback):
    return callback(lambda value: 1 if value else 0)
