from py.chainpkg import nested


def test_imported_module_chain() -> object:
    return nested.module.target()
