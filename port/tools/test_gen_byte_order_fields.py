"""The committed byte_order_fields.h is what gen_byte_order_fields.py writes."""

import unittest

import gen_byte_order_fields as gen


class GeneratedHeader(unittest.TestCase):
    def test_the_committed_header_is_the_generator_output(self):
        self.assertEqual(gen.HEADER.read_text(encoding="utf-8"), gen.render())

    def test_every_line_fits_the_column_limit(self):
        for line in gen.render().splitlines():
            self.assertLessEqual(len(line), gen.COLUMNS, line)


if __name__ == "__main__":
    unittest.main()
