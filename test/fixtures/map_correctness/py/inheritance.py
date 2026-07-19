from py.inheritance_base import ExternalBase


class Base:
    def step(self) -> str:
        return "base"


class Other:
    def step(self) -> str:
        return "other"


class Child(Base):
    pass


class Override(Base):
    def step(self) -> str:
        return "override"


class Ambiguous(Base, Other):
    pass


class ExternalChild(ExternalBase):
    pass
