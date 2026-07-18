from py.widget import Receiver, Widget, Wrapper


def test_imported_member() -> str:
    return Widget.build()


def test_constructed_receiver() -> str:
    receiver = Receiver()
    return receiver.run()


def test_chained_receiver() -> str:
    receiver = Receiver.create().chain()
    return receiver.run()


def test_reassigned_receiver() -> str:
    receiver = Receiver()
    receiver = object()
    return receiver.run()


@Wrapper
def test_decorated_callable() -> str:
    return "decorated"
