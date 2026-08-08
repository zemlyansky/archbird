from py.widget import Receiver, Widget, Wrapper


class BaseWidget:
    @staticmethod
    def inherited() -> str:
        return "inherited"


class LocalWidget(BaseWidget):
    @staticmethod
    def build() -> str:
        return "built"

    def run(self) -> str:
        return self.build()

    @classmethod
    def identify(cls) -> str:
        return cls.build()


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


def test_local_static_member() -> str:
    return LocalWidget.build()


def test_local_constructed_receiver() -> str:
    return LocalWidget().run()


def test_local_inherited_member() -> str:
    return LocalWidget.inherited()


def test_local_nested_duplicate() -> str:
    class LocalWidget:
        @staticmethod
        def build() -> str:
            return "nested"

    return LocalWidget.build()


def test_local_shadowed_class() -> object:
    LocalWidget = object
    return LocalWidget.build()


@Wrapper
def test_decorated_callable() -> str:
    return "decorated"
