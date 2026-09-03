from rootpkg.configuration import Configuration


class Service:
    def __init__(self, configuration: Configuration) -> None:
        self.configuration = configuration

    def invoke(self) -> list[str]:
        return self.configuration.controller_arguments()


class ReassignedService:
    def __init__(self, configuration: Configuration) -> None:
        self.configuration = configuration
        self.configuration = object()

    def invoke(self) -> list[str]:
        return self.configuration.controller_arguments()
