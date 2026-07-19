from py.inheritance import Ambiguous, Child, ExternalChild, Override


def test_inherited_step() -> str:
    child = Child()
    return child.step()


def test_overridden_step() -> str:
    child = Override()
    return child.step()


def test_ambiguous_step() -> str:
    child = Ambiguous()
    return child.step()


def test_imported_inherited_step() -> str:
    child = ExternalChild()
    return child.step()


def test_closed_over_inherited_step() -> str:
    child = Child()

    def run() -> str:
        return child.step()

    return run()
