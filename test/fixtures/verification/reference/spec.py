from enum import Enum, auto


class Ops(Enum):
    ADD = auto()
    MUL = auto()
    WAIT = auto()


class GroupOp:
    Required = {Ops.ADD, Ops.WAIT}
