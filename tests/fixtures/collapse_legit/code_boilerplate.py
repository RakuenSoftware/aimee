# shape: repeated import/test-stub boilerplate; expected: no-fire; expected_loop_start_offset: -1; expected_loop_span_bytes: -1
import os
import sys

def test_ready():
    assert True

import os
import sys

def test_ready_again():
    assert True
