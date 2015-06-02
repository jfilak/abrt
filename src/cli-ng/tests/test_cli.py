#!/usr/bin/python
# -*- encoding: utf-8 -*-
import logging
try:
    import unittest2 as unittest
except ImportError:
    import unittest

import clitests

from abrtcli.cli import main, info
from abrtcli.utils import captured_output


class CliTestCase(clitests.TestCase):
    '''
    Tests for cli functions
    '''

    def test_cli_sanity(self):
        '''
        Test if main works and there are no import/decorator errors
        '''

        with captured_output() as (cap_stdout, cap_stderr):
            with self.assertRaises(SystemExit):
                main()

        stderr = cap_stderr.getvalue()
        self.assertIn("usage", stderr)
        self.assertIn("debuginfo-install", stderr)


if __name__ == '__main__':
    logging.basicConfig(level=logging.INFO)
    unittest.main()
