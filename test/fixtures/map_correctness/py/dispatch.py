def build_alpha() -> str:
    return "alpha"


def build_beta() -> str:
    return "beta_2d"


CASES: dict[str, object] = {
    "alpha": build_alpha,
    "beta_2d": build_beta,
}
