import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("export-kitchen.py")
SPEC = importlib.util.spec_from_file_location("export_kitchen", MODULE_PATH)
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)


class StationGridAlignmentTests(unittest.TestCase):
    def test_snaps_transform_noise_on_grid_coordinate(self):
        self.assertEqual(EXPORT.snap_near_grid(-7.19, -8.4, 0.6), -7.2)

    def test_preserves_material_station_offset(self):
        self.assertEqual(EXPORT.snap_near_grid(5.06, -8.4, 0.6), 5.06)

    def test_without_grid_origin_only_rounds_precision(self):
        self.assertEqual(EXPORT.snap_near_grid(-7.196, None, 0.6), -7.2)


if __name__ == "__main__":
    unittest.main()
