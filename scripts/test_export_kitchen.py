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

    def test_cook_node_carries_required_step(self):
        menu = {"dishes": [{"dish": "Rice", "steps": [{
            "node": "BoiledRice", "type": "CookedCompositeOrderNode", "cook": "Pot",
            "children": [{"node": "Rice", "type": "IngredientOrderNode"}]}]}]}
        recipes = EXPORT.build_recipes(menu, {"stations": []}, ["cooker"])
        steps = {n["id"]: n.get("step") for n in recipes["nodes"]}
        self.assertEqual(steps["cook:BoiledRice"], "Pot")
        self.assertIsNone(steps["fetch:Rice"])



if __name__ == "__main__":
    unittest.main()
