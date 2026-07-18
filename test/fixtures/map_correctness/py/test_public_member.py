from py.publicpkg import Alias, Gadget


def test_reexported_member() -> str:
    return Gadget.make()


def test_aliased_member() -> str:
    return Alias.make()
