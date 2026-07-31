import unittest

from api import add


class Backend:
    @staticmethod
    def core_sum(left, right):
        return left + right


class ApiTest(unittest.TestCase):
    def test_add(self):
        self.assertEqual(add(Backend(), 2, 3), 5)


if __name__ == "__main__":
    unittest.main()
