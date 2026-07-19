from py.publicpkg import Alias, Gadget


def test_reexported_member() -> str:
    return Gadget.make()


def test_aliased_member() -> str:
    return Alias.make()


def test_reexported_constructor() -> bool:
    return Gadget().ready


def test_aliased_constructor() -> bool:
    return Alias().ready
