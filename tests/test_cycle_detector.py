import unittest
import sys
import os

# offline_cycle_check.py lives next to this test
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from offline_cycle_check import WaitForGraph, parse_log_file

class TestCycleDetector(unittest.TestCase):
    def test_simple_deadlock(self):
        g = WaitForGraph()
        g.process_event('HOLD', '1', 'A')
        g.process_event('HOLD', '2', 'B')
        g.process_event('WAIT', '1', 'B')
        g.process_event('WAIT', '2', 'A')
        cycle = g.detect_cycle()
        self.assertIsNotNone(cycle)
        self.assertTrue(len(cycle) > 0)

    def test_no_deadlock_consistent_order(self):
        g = WaitForGraph()
        g.process_event('WAIT', '1', 'A')
        g.process_event('HOLD', '1', 'A')
        g.process_event('WAIT', '1', 'B')
        g.process_event('HOLD', '1', 'B')
        g.process_event('RELEASE', '1', 'B')
        g.process_event('RELEASE', '1', 'A')
        g.process_event('WAIT', '2', 'A')
        g.process_event('HOLD', '2', 'A')
        g.process_event('WAIT', '2', 'B')
        g.process_event('HOLD', '2', 'B')
        g.process_event('RELEASE', '2', 'B')
        g.process_event('RELEASE', '2', 'A')
        cycle = g.detect_cycle()
        self.assertIsNone(cycle)

    def test_three_way_cycle(self):
        g = WaitForGraph()
        g.process_event('HOLD', '1', 'A')
        g.process_event('HOLD', '2', 'B')
        g.process_event('HOLD', '3', 'C')
        g.process_event('WAIT', '1', 'B')
        g.process_event('WAIT', '2', 'C')
        g.process_event('WAIT', '3', 'A')
        cycle = g.detect_cycle()
        self.assertIsNotNone(cycle)

    def test_single_thread_no_cycle(self):
        g = WaitForGraph()
        g.process_event('HOLD', '1', 'A')
        g.process_event('WAIT', '1', 'B')
        g.process_event('HOLD', '1', 'B')
        g.process_event('RELEASE', '1', 'B')
        g.process_event('RELEASE', '1', 'A')
        cycle = g.detect_cycle()
        self.assertIsNone(cycle)

    def test_release_breaks_cycle(self):
        g = WaitForGraph()
        g.process_event('HOLD', '1', 'A')
        g.process_event('HOLD', '2', 'B')
        g.process_event('WAIT', '1', 'B')
        g.process_event('RELEASE', '2', 'B') # Thread 2 releases B before waiting for A
        g.process_event('WAIT', '2', 'A')
        cycle = g.detect_cycle()
        self.assertIsNone(cycle)

    def test_diamond_no_cycle(self):
        # A depends on B and C, B depends on D, C depends on D
        g = WaitForGraph()
        g.add_edge('A', 'B')
        g.add_edge('A', 'C')
        g.add_edge('B', 'D')
        g.add_edge('C', 'D')
        cycle = g.detect_cycle()
        self.assertIsNone(cycle)

    def test_parse_deadlock_log(self):
        log_path = os.path.join(os.path.dirname(__file__), 'sample_logs', 'deadlock_sample.log')
        cycle = parse_log_file(log_path)
        self.assertIsNotNone(cycle)

    def test_parse_clean_log(self):
        log_path = os.path.join(os.path.dirname(__file__), 'sample_logs', 'clean_sample.log')
        cycle = parse_log_file(log_path)
        self.assertIsNone(cycle)

if __name__ == '__main__':
    unittest.main()
