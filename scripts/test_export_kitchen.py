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


class AssembleStationTests(unittest.TestCase):
    MENU = {"dishes": [{"dish": "Sushi", "steps": [{
        "node": "Sushi", "type": "CompositeOrderNode",
        "children": [{"node": "Prawn", "type": "IngredientOrderNode", "plate": "Plate"}]}]}]}

    def assemble_resource(self, kinds):
        recipes = EXPORT.build_recipes(self.MENU, {"stations": []}, kinds)
        return {n["id"]: n["resource"] for n in recipes["nodes"]}["assemble:Sushi"]

    def test_plated_assemble_goes_to_plate_stack(self):
        self.assertEqual(self.assemble_resource(["plate_stack", "container"]), "plate_stack")

    def test_without_plate_stack_stays_on_container(self):
        self.assertEqual(self.assemble_resource(["sink", "container"]), "container")


class CookerStepTests(unittest.TestCase):
    RAW = [
        {"type": "ClientCookingStation", "name": "FryingStation", "pos": [-3.0, -3.6]},
        {"type": "ClientCookableContainer", "name": "FrierBasket", "pos": [-3.0, -3.6],
         "step": "DeepFatFryer"},
        {"type": "ClientCookingStation", "name": "workstation_cooker_01", "pos": [6.6, -6.0]},
        {"type": "ClientCookableContainer", "name": "utensil_pot_01", "pos": [6.6, -6.0],
         "step": "Pot"},
        {"type": "ClientCookingStation", "name": "workstation_cooker_01", "pos": [1.8, -6.0]},
    ]

    def test_cooker_takes_step_of_utensil_on_it(self):
        stations, _ = EXPORT.build_stations(self.RAW, 0.6, (None, None))
        cookers = {tuple(s["pos"]): s.get("step") for s in stations if s["kind"] == "cooker"}
        self.assertEqual(cookers, {(-3.0, -3.6): "DeepFatFryer", (6.6, -6.0): "Pot",
                                   (1.8, -6.0): None})
        self.assertEqual([s["kind"] for s in stations].count("cooker"), 3)
        self.assertNotIn("container", [s["kind"] for s in stations])

    def test_cook_node_carries_required_step(self):
        menu = {"dishes": [{"dish": "Rice", "steps": [{
            "node": "BoiledRice", "type": "CookedCompositeOrderNode", "cook": "Pot",
            "children": [{"node": "Rice", "type": "IngredientOrderNode"}]}]}]}
        recipes = EXPORT.build_recipes(menu, {"stations": []}, ["cooker"])
        steps = {n["id"]: n.get("step") for n in recipes["nodes"]}
        self.assertEqual(steps["cook:BoiledRice"], "Pot")
        self.assertIsNone(steps["fetch:Rice"])



class BoardStationTests(unittest.TestCase):
    RAW = [
        {"type": "ClientWorkstation", "name": "ChoppingCounter", "pos": [6.6, -1.2]},
        {"type": "ClientWorkstation", "name": "ChoppingCounter", "pos": [3.0, -1.2]},
        {"type": "ClientWorkstation", "name": "FryingStation", "pos": [1.0, 0.0]},
    ]

    def test_boards_exported_sorted_by_position(self):
        stations, _ = EXPORT.build_stations(self.RAW, 0.6, (None, None))
        boards = [s for s in stations if s["kind"] == "board"]
        self.assertEqual([s["id"] for s in boards], ["board-0", "board-1"])
        self.assertEqual([s["pos"] for s in boards], [[3.0, -1.2], [6.6, -1.2]])
        self.assertTrue(all("ClientWorkstation" in s["types"] for s in boards))

    def test_non_chopping_workstation_is_not_a_board(self):
        stations, unknown = EXPORT.build_stations(self.RAW, 0.6, (None, None))
        self.assertFalse(any(s["name"] == "FryingStation" for s in stations))
        self.assertNotIn("ClientWorkstation", unknown)


class UtensilTests(unittest.TestCase):
    COOKER = {"type": "ClientCookingStation", "name": "workstation_cooker_01",
              "pos": [6.6, -6.0]}
    UTENSIL = {"type": "ClientCookableContainer", "name": "utensil_pot_01",
               "pos": [6.6, -6.0], "step": "Pot",
               "contents": [{"name": "Rice"}]}
    UTENSIL_BODY = {"type": "ClientIngredientContainer", "name": "utensil_pot_01",
                    "pos": [6.6, -6.0], "contents": [{"name": "Rice"}]}

    def test_utensil_is_attribute_not_station(self):
        stations, _ = EXPORT.build_stations(
            [self.COOKER, self.UTENSIL_BODY, self.UTENSIL], 0.6, (None, None))
        self.assertEqual([s["kind"] for s in stations], ["cooker"])
        self.assertEqual(stations[0]["id"], "cooker-0")
        self.assertEqual(stations[0]["step"], "Pot")

    def test_plain_ingredient_container_still_exported(self):
        tray = {"type": "ClientIngredientContainer", "name": "TortillaTray",
                "pos": [1.8, 1.8]}
        stations, _ = EXPORT.build_stations([tray], 0.6, (None, None))
        self.assertEqual([s["kind"] for s in stations], ["container"])
        self.assertEqual(stations[0]["id"], "container-0")


if __name__ == "__main__":
    unittest.main()
