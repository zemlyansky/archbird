from pkg.api import main
import archbird_modern.missing


def test_main():
    assert main() == 1


class TestNested:
    def test_nested(self):
        assert main()
