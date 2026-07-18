import py.candidatepkg as candidate


def test_unproven_module_chain() -> object:
    return candidate.hidden.target()
