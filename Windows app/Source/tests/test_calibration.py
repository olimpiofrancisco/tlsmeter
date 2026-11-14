"""Testes sem hardware nem interface gráfica: python -m unittest discover -s tests -v"""
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'Windows'))
from calibration_store import (
    CalibrationData, calibrated_values, check_reference, compute_factor,
    load_calibration, new_calibration, save_calibration,
)


class CalibrationTests(unittest.TestCase):
    def test_example(self):
        record = new_calibration(.30, .25, 'PA66-Corpo-01')
        self.assertAlmostEqual(record.factor, 1.2)
        self.assertAlmostEqual(calibrated_values(.25, 4.0, record.factor)[0], .30)
        self.assertAlmostEqual(calibrated_values(.25, 4.0, record.factor)[1], 4/1.2)

    def test_missing_reference(self):
        with self.assertRaises(ValueError):
            check_reference(0, 'PA66')

    def test_missing_sample_identification(self):
        with self.assertRaises(ValueError):
            check_reference(.30, '')

    def test_reference_range(self):
        for ref in [-1, 0, .009, 10.001, 100]:
            with self.subTest(ref=ref):
                with self.assertRaises(ValueError): check_reference(ref, 'PA66')

    def test_nan_infinite(self):
        for value in [math.nan, math.inf, '-inf', 'abc']:
            with self.subTest(value=value):
                with self.assertRaises(ValueError): compute_factor(value, .3)

    def test_zero_raw(self):
        with self.assertRaises(ValueError): compute_factor(.30, 0)

    def test_outlier_factor(self):
        with self.assertRaises(ValueError): compute_factor(10, .01)

    def test_no_calibration(self):
        self.assertEqual(calibrated_values(.30, 3.3333, 1), (.30, 3.3333))

    def test_partial_results(self):
        self.assertEqual(calibrated_values(None, 4, 2), (None, 2))
        self.assertEqual(calibrated_values(.2, None, 2), (.4, None))

    def test_store_roundtrip(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'sub' / 'cal.json'
            record = new_calibration(.30, .25, 'PA66-corpo01')
            save_calibration(record, path)
            back = load_calibration(path)
            self.assertEqual(back.sample_id, 'PA66-corpo01')
            self.assertAlmostEqual(back.factor, 1.2)
            self.assertTrue(back.enabled)
            back.enabled = False
            save_calibration(back, path)
            self.assertFalse(load_calibration(path).enabled)

    def test_invalid_or_missing_store(self):
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'cal.json'
            self.assertFalse(load_calibration(path).valid)
            path.write_text('{invalid', encoding='utf-8')
            self.assertFalse(load_calibration(path).valid)
            path.write_text('[]', encoding='utf-8')
            self.assertFalse(load_calibration(path).valid)

    def test_malformed_factor(self):
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'cal.json'
            path.write_text(json.dumps(dict(factor=math.nan, valid=True)), encoding='utf-8')
            self.assertFalse(load_calibration(path).valid)

    def test_raw_values_immutable(self):
        k_raw=.25; rho_raw=4.0
        out=calibrated_values(k_raw,rho_raw,1.2)
        self.assertEqual((k_raw,rho_raw),(.25,4.0))
        self.assertEqual(out,(.30,4/1.2))


if __name__=='__main__':
    unittest.main()
