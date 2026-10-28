import importlib.util
import json
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("export-kitchen.py")
SPEC = importlib.util.spec_from_file_location("export_kitchen", MODULE_PATH)
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)

FIXTURES = pathlib.Path(__file__).with_name("fixtures") / "s_wellcooked_demo_3"


class StationGridAlignmentTests(unittest.TestCase):
    def test_snaps_transform_noise_on_grid_coordinate(self):
        self.assertEqual(EXPORT.snap_near_grid(-7.19, -8.4, 0.6), -7.2)

    def test_preserves_material_station_offset(self):
        self.assertEqual(EXPORT.snap_near_grid(5.06, -8.4, 0.6), 5.06)

    def test_without_grid_origin_only_rounds_precision(self):
        self.assertEqual(EXPORT.snap_near_grid(-7.196, None, 0.6), -7.2)


class SushiShapeTests(unittest.TestCase):
    """寿司：一份切好的料 → 盘堆 → 取盘 → 交付，没有独立的 assemble。"""
    MENU = {"dishes": [{"dish": "Sushi", "steps": [{
        "node": "Sushi", "type": "CompositeOrderNode",
        "children": [{"node": "Prawn", "type": "IngredientOrderNode", "plate": "Plate"}]}]}]}
    STATE = {"stations": [{"type": "ClientPickupItemSpawner", "name": "crate",
                           "pos": [0.0, 0.0], "gives": "Prawn",
                           "prefab_components": ["WorkableItem"]}]}

    def test_sushi_has_cut_then_plate_without_assemble(self):
        recipes = EXPORT.build_recipes(self.MENU, self.STATE, ["plate_stack", "board"])
        nodes = {n["id"]: n for n in recipes["nodes"]}
        self.assertNotIn("assemble:Sushi", nodes)
        self.assertEqual(sorted(nodes), ["cut:Prawn", "deliver:Sushi", "fetch:Prawn",
                                         "plate:Sushi"])
        self.assertEqual(nodes["cut:Prawn"]["resource"], "board")
        self.assertEqual(nodes["fetch:Prawn"]["ingredient"], "prawn")
        self.assertNotIn("workable", nodes["fetch:Prawn"])
        self.assertEqual(recipes["dishes"][0]["nodes"],
                         ["fetch:Prawn", "cut:Prawn", "plate:Sushi", "deliver:Sushi"])

    def test_leaf_without_workable_is_not_cut(self):
        menu = {"dishes": [{"dish": "Plain", "steps": [{
            "node": "Plain", "type": "CompositeOrderNode",
            "children": [{"node": "Tortilla", "type": "IngredientOrderNode",
                          "plate": "Plate"}]}]}]}
        state = {"stations": [{"type": "ClientPickupItemSpawner", "name": "crate",
                               "pos": [0.0, 0.0], "gives": "Tortilla",
                               "prefab_components": ["PreparationContainer"]}]}
        recipes = EXPORT.build_recipes(menu, state, ["plate_stack"])
        self.assertEqual(recipes["dishes"][0]["nodes"],
                         ["fetch:Tortilla", "plate:Plain", "deliver:Plain"])


class BurritoShapeTests(unittest.TestCase):
    """卷饼：玉米饼上台面 → 每份熟料从锅倒进去 → 取玉米饼 → 盘堆 → 交付。"""

    def menu(self, dish="Burrito"):
        return {"dishes": [{"dish": dish, "steps": [{
            "node": dish, "type": "CompositeOrderNode",
            "children": [
                {"node": "Tortilla", "type": "IngredientOrderNode", "plate": "Plate"},
                {"node": "BoiledRice", "type": "CookedCompositeOrderNode", "cook": "Pot",
                 "children": [{"node": "Rice", "type": "IngredientOrderNode"}]},
                {"node": "FriedMeat", "type": "CookedCompositeOrderNode", "cook": "FryingPan",
                 "children": [{"node": "Meat", "type": "IngredientOrderNode"}]},
            ]}]}]}

    def state(self):
        return {"stations": [
            {"type": "ClientPickupItemSpawner", "name": "crate", "pos": [0.0, 0.0],
             "gives": "Meat", "prefab_components": ["WorkableItem"]},
            {"type": "ClientPickupItemSpawner", "name": "crate", "pos": [0.6, 0.0],
             "gives": "Rice", "prefab_components": ["CookableProperties"]},
            {"type": "ClientPickupItemSpawner", "name": "crate", "pos": [1.2, 0.0],
             "gives": "Tortilla", "prefab_components": ["PreparationContainer"]},
        ]}

    def test_burrito_sequence(self):
        recipes = EXPORT.build_recipes(self.menu(), self.state(),
                                       ["plate_stack", "board", "counter", "cooker"])
        self.assertEqual(recipes["dishes"][0]["nodes"], [
            "fetch:Meat", "fetch:Rice", "fetch:Tortilla", "cut:Meat", "cook:BoiledRice",
            "cook:FriedMeat", "place:Tortilla", "pour:Meat", "pour:Rice",
            "pickup:Burrito", "plate:Burrito", "deliver:Burrito"])

    def test_pour_edges_point_at_place_and_cook(self):
        recipes = EXPORT.build_recipes(self.menu(), self.state(), ["counter"])
        edges = {tuple(edge) for edge in recipes["edges"]}
        self.assertIn(("place:Tortilla", "pour:Meat"), edges)
        self.assertIn(("cook:FriedMeat", "pour:Meat"), edges)
        self.assertIn(("place:Tortilla", "pour:Rice"), edges)
        self.assertIn(("cook:BoiledRice", "pour:Rice"), edges)
        self.assertIn(("pour:Meat", "pickup:Burrito"), edges)
        self.assertIn(("pour:Rice", "pickup:Burrito"), edges)
        self.assertIn(("pickup:Burrito", "plate:Burrito"), edges)
        self.assertIn(("plate:Burrito", "deliver:Burrito"), edges)

    def test_pour_nodes_carry_the_pan_content_name(self):
        recipes = EXPORT.build_recipes(self.menu(), self.state(), ["counter"])
        nodes = {n["id"]: n for n in recipes["nodes"]}
        self.assertEqual(nodes["pour:Meat"]["resource"], "counter")
        self.assertEqual(nodes["pour:Rice"]["resource"], "counter")
        self.assertEqual(nodes["place:Tortilla"]["resource"], "counter")
        self.assertEqual(nodes["pickup:Burrito"]["resource"], "counter")

    def test_pickup_is_per_dish(self):
        menu = self.menu("First")
        menu["dishes"].append(self.menu("Second")["dishes"][0])
        recipes = EXPORT.build_recipes(menu, self.state(), ["counter"])
        ids = {n["id"] for n in recipes["nodes"]}
        self.assertIn("pickup:First", ids)
        self.assertIn("pickup:Second", ids)
        # 共享的中间产物只出现一次
        self.assertEqual(sum(1 for node_id in ids if node_id == "pour:Rice"), 1)
        self.assertEqual(sum(1 for node_id in ids if node_id == "place:Tortilla"), 1)

    def test_durations_come_from_the_measurement(self):
        recipes = EXPORT.build_recipes(self.menu(), self.state(), ["counter"])
        durations = {n["id"]: n["duration"] for n in recipes["nodes"]}
        self.assertEqual(durations["cut:Meat"], 1.7)
        self.assertEqual(durations["cook:FriedMeat"], 12.1)
        self.assertEqual(durations["pour:Meat"], 0.1)
        self.assertEqual(durations["plate:Burrito"], 0.1)
        self.assertEqual(durations["deliver:Burrito"], 0.5)
        self.assertIn("logs/manual/20261027-022317", recipes["durations"]["source"])
        self.assertTrue(any("16.1 s" in note for note in recipes["notes"]))

    def test_cook_nodes_carry_the_station_step(self):
        recipes = EXPORT.build_recipes(self.menu(), self.state(), ["cooker"])
        steps = {n["id"]: n.get("step") for n in recipes["nodes"]}
        self.assertEqual(steps["cook:FriedMeat"], "FryingPan")
        self.assertEqual(steps["cook:BoiledRice"], "Pot")


class LegacyShapeTests(unittest.TestCase):
    """其它形状（旧地图菜单）仍按树直译、assemble 收口。"""
    MENU = {"dishes": [{"dish": "Soup", "steps": [{
        "node": "Soup", "type": "CompositeOrderNode",
        "children": [
            {"node": "BoiledTomato", "type": "CookedCompositeOrderNode", "cook": "Pot",
             "children": [{"node": "Tomato", "type": "IngredientOrderNode"}]},
            {"node": "Water", "type": "IngredientOrderNode"},
        ]}]}]}

    def test_fallback_assembles_on_container(self):
        recipes = EXPORT.build_recipes(self.MENU, {"stations": []}, ["container"])
        self.assertIn("assemble:Soup", {n["id"] for n in recipes["nodes"]})

    def test_fallback_assembles_on_plate_stack_when_a_child_is_plated(self):
        menu = {"dishes": [{"dish": "Dish", "steps": [{
            "node": "Dish", "type": "CompositeOrderNode",
            "children": [
                {"node": "Water", "type": "IngredientOrderNode", "plate": "Plate"},
                {"node": "BoiledTomato", "type": "CookedCompositeOrderNode", "cook": "Pot",
                 "children": [{"node": "Tomato", "type": "IngredientOrderNode"}]},
            ]}]}]}
        recipes = EXPORT.build_recipes(menu, {"stations": []}, ["plate_stack", "container"])
        resources = {n["id"]: n["resource"] for n in recipes["nodes"]}
        self.assertEqual(resources["assemble:Dish"], "plate_stack")


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


class CounterStationTests(unittest.TestCase):
    """普通台面：卷饼当容器的地方。只有名字含 countertop 且不是砧板的 AttachStation 才算。"""

    def test_plain_countertop_is_a_counter(self):
        raw = [{"type": "ClientAttachStation", "name": "countertop_01_standard_wood",
                "pos": [0.6, -6.0]}]
        stations, unknown = EXPORT.build_stations(raw, 0.6, (None, None))
        self.assertEqual([s["kind"] for s in stations], ["counter"])
        self.assertEqual(stations[0]["id"], "counter-0")
        self.assertNotIn("ClientAttachStation", unknown)

    def test_chopping_board_stays_a_board(self):
        raw = [
            {"type": "ClientWorkstation",
             "name": "countertop_01_chopping_board_wood_no_edge", "pos": [3.0, -1.2]},
            {"type": "ClientAttachStation",
             "name": "countertop_01_chopping_board_wood_no_edge", "pos": [3.0, -1.2]},
        ]
        stations, _ = EXPORT.build_stations(raw, 0.6, (None, None))
        self.assertEqual([s["kind"] for s in stations], ["board"])

    def test_crate_attach_station_is_not_a_counter(self):
        raw = [
            {"type": "ClientPickupItemSpawner", "name": "dispenser_crate_01",
             "pos": [0.6, -8.4], "gives": "Rice"},
            {"type": "ClientAttachStation", "name": "dispenser_crate_01", "pos": [0.6, -8.4]},
        ]
        stations, _ = EXPORT.build_stations(raw, 0.6, (None, None))
        self.assertEqual([s["kind"] for s in stations], ["dispenser"])


class StandCellTests(unittest.TestCase):
    """没有可站格的工位必须被导出丢掉：留着会让 `kos_stand_build` 拒绝整张图。"""

    # 一条 5 格的走廊（z=0），台面摆在走廊端头外侧，只有 -x 侧有格可站。
    CELLS = [[0.0, 0.0], [0.6, 0.0], [1.2, 0.0], [1.8, 0.0], [2.4, 0.0]]

    def test_stand_cells_follow_the_four_directions(self):
        # 台面嵌在走廊的空格里（x=1.2 不在 cells 里）：左右两格都正对。
        cells = [[0.0, 0.0], [1.2, 0.0], [2.4, 0.0]]
        self.assertEqual(sorted(EXPORT.stand_cells([0.6, 0.0], cells, 0.6)), [0, 1])
        # 只要有一条格线正对，多远都算站位（C 侧 stand.c 也是取最近的格，不设上限）。
        self.assertEqual(EXPORT.stand_cells([1.2, 3.0], cells, 0.6), [1])
        # 横竖两条格线上都没有格才是没有站位（`_3` 地图四角就是这种）。
        self.assertEqual(EXPORT.stand_cells([4.2, 3.0], cells, 0.6), [])

    def test_counter_without_stand_is_dropped_and_renumbered(self):
        raw = [
            {"type": "ClientAttachStation", "name": "countertop_01_standard_wood",
             "pos": [4.2, 3.0]},   # 横竖都没有格线，没有站位
            {"type": "ClientAttachStation", "name": "countertop_01_standard_wood",
             "pos": [0.6, 0.0]},
            {"type": "ClientAttachStation", "name": "countertop_01_standard_wood",
             "pos": [1.8, 0.0]},
        ]
        stations, _ = EXPORT.build_stations(raw, 0.6, (None, None))
        kept, dropped = EXPORT.drop_stations_without_stand(stations, self.CELLS, 0.6)
        self.assertEqual([s["pos"] for s in dropped], [[4.2, 3.0]])
        self.assertEqual([s["id"] for s in kept], ["counter-0", "counter-1"])
        self.assertEqual([s["pos"] for s in kept], [[0.6, 0.0], [1.8, 0.0]])

    def test_drop_keeps_a_station_that_has_any_direction(self):
        stations = [{"kind": "counter", "id": "counter-0", "pos": [0.6, 0.0],
                     "name": "countertop_01_standard_wood"}]
        kept, dropped = EXPORT.drop_stations_without_stand(stations, self.CELLS, 0.6)
        self.assertEqual(dropped, [])
        self.assertEqual([s["id"] for s in kept], ["counter-0"])


class Demo3FixtureTests(unittest.TestCase):
    """`_3` 的实机夹具：5 道菜、38 张普通台面、每个节点都有工位。

    夹具来自 20261027-033435 那次实机导出（state/menu/terrain 原样）加新插件会发出的
    42 条 `ClientAttachStation` 记录（对象名见 manual.log，坐标见 layout_3.tsv）。
    kitchen.json / recipes.json 由 `scripts/export-kitchen.py` 从这三份文件生成，
    与 Runner 进关后跑的是同一个生成器。
    """

    @classmethod
    def setUpClass(cls):
        with open(FIXTURES / "menu.json", encoding="utf-8") as handle:
            cls.menu = json.load(handle)
        with open(FIXTURES / "state.json", encoding="utf-8") as handle:
            cls.state = json.load(handle)
        with open(FIXTURES / "terrain.json", encoding="utf-8") as handle:
            cls.terrain = json.load(handle)
        cls.step = cls.terrain["step"]
        cls.cells = [[round(x, 2), round(z, 2)] for x, z in cls.terrain["cells"]]
        cls.origin = (min(x for x, _ in cls.cells), min(z for _, z in cls.cells))
        cls.stations, cls.unknown = EXPORT.build_stations(
            cls.state["stations"], cls.step, cls.origin)
        cls.stations, cls.dropped = EXPORT.drop_stations_without_stand(
            cls.stations, cls.cells, cls.step)
        cls.kinds = sorted({s["kind"] for s in cls.stations})
        cls.recipes = EXPORT.build_recipes(cls.menu, cls.state, cls.kinds)
        cls.dishes = {d["dish"]: d for d in cls.recipes["dishes"]}
        cls.nodes = {n["id"]: n for n in cls.recipes["nodes"]}

    def test_checked_in_generated_files_match_the_fixture(self):
        """test_recipe_stations.c 直接读这两份文件，生成器改了它们必须一起更新。"""
        with open(FIXTURES / "kitchen.json", encoding="utf-8") as handle:
            kitchen = json.load(handle)
        with open(FIXTURES / "recipes.json", encoding="utf-8") as handle:
            recipes = json.load(handle)
        self.assertEqual([(s["id"], s["kind"], s["pos"]) for s in kitchen["stations"]],
                         [(s["id"], s["kind"], s["pos"]) for s in self.stations])
        self.assertEqual(kitchen["kinds"], {k: sum(1 for s in self.stations if s["kind"] == k)
                                           for k in dict.fromkeys(s["kind"] for s in self.stations)})
        self.assertEqual(recipes["dishes"], self.recipes["dishes"])
        self.assertEqual(recipes["nodes"], self.recipes["nodes"])

    def test_station_counts_match_the_level(self):
        counts = {}
        for station in self.stations:
            counts[station["kind"]] = counts.get(station["kind"], 0) + 1
        self.assertEqual(counts, {"serving": 1, "dispenser": 7, "cooker": 6, "board": 2,
                                  "sink": 1, "clean_plate_stack": 1, "plate_stack": 1,
                                  "counter": 38})
        self.assertEqual(self.unknown, [])

    def test_counters_without_stand_are_dropped(self):
        # 地图四角各有一张被围住的台面，站位格取不到；留着会让 kos_stand_build 拒绝整张图。
        self.assertEqual(sorted(tuple(s["pos"]) for s in self.dropped),
                         [(-3.0, -8.4), (-3.0, 1.2), (12.6, -8.4), (12.6, 1.2)])

    def test_counter_under_plate_stack_is_exported_but_not_free(self):
        under_stack = [s for s in self.stations
                       if s["kind"] == "counter" and tuple(s["pos"]) == (3.0, 1.2)]
        self.assertEqual(len(under_stack), 1)
        # 同格还有盘堆：Scheduler 的 kos_kitchen_pick_counter 会跳过它。
        self.assertIn("plate_stack", {s["kind"] for s in self.stations
                                      if tuple(s["pos"]) == (3.0, 1.2)})

    def test_every_node_of_every_dish_has_a_station(self):
        kinds = {}
        for station in self.stations:
            kinds.setdefault(station["kind"], []).append(station)
        cookers = kinds.get("cooker", [])
        for node in self.recipes["nodes"]:
            resource = node.get("resource")
            if resource and node["op"] != "drop":
                self.assertIn(resource, kinds, node["id"])
            if node["op"] == "cook" and node.get("step"):
                self.assertTrue(any(st.get("step") in (None, "", node["step"])
                                    for st in cookers), node["id"])
        for dish in self.recipes["dishes"]:
            for node_id in dish["nodes"]:
                self.assertIn(node_id, self.nodes)
                node = self.nodes[node_id]
                if node["op"] == "place":
                    self.assertEqual(node["resource"], "counter")

    def test_chicken_burrito_sequence(self):
        self.assertEqual(self.dishes["Chicken_Burrito"]["nodes"], [
            "fetch:BurritoChicken", "fetch:Rice", "fetch:Tortilla", "cut:BurritoChicken",
            "cook:BoiledRice", "cook:FriedChicken", "place:Tortilla", "pour:BurritoChicken",
            "pour:Rice", "pickup:Chicken_Burrito", "plate:Chicken_Burrito",
            "deliver:Chicken_Burrito"])

    def test_meat_burrito_sequence(self):
        self.assertEqual(self.dishes["Meat_Burrito"]["nodes"], [
            "fetch:BurritoMeat", "fetch:Rice", "fetch:Tortilla", "cut:BurritoMeat",
            "cook:BoiledRice", "cook:FriedBurritoMeat", "place:Tortilla", "pour:BurritoMeat",
            "pour:Rice", "pickup:Meat_Burrito", "plate:Meat_Burrito", "deliver:Meat_Burrito"])

    def test_mushroom_burrito_sequence(self):
        self.assertEqual(self.dishes["Mushroom_Burrito"]["nodes"], [
            "fetch:Mushroom", "fetch:Rice", "fetch:Tortilla", "cut:Mushroom",
            "cook:BoiledRice", "cook:FriedMushrooms", "place:Tortilla", "pour:Mushroom",
            "pour:Rice", "pickup:Mushroom_Burrito", "plate:Mushroom_Burrito",
            "deliver:Mushroom_Burrito"])

    def test_sushi_sequences(self):
        self.assertEqual(self.dishes["Sushi_PlainFish"]["nodes"],
                         ["fetch:SushiFish", "cut:SushiFish", "plate:Sushi_PlainFish",
                          "deliver:Sushi_PlainFish"])
        self.assertEqual(self.dishes["Sushi_PlainPrawn"]["nodes"],
                         ["fetch:SushiPrawn", "cut:SushiPrawn", "plate:Sushi_PlainPrawn",
                          "deliver:Sushi_PlainPrawn"])

    def test_all_five_dishes_present(self):
        self.assertEqual(sorted(self.dishes),
                         ["Chicken_Burrito", "Meat_Burrito", "Mushroom_Burrito",
                          "Sushi_PlainFish", "Sushi_PlainPrawn"])

    def test_shared_burrito_nodes_appear_in_all_three(self):
        for node_id in ("fetch:Rice", "cook:BoiledRice", "pour:Rice", "fetch:Tortilla",
                        "place:Tortilla"):
            for dish in ("Chicken_Burrito", "Meat_Burrito", "Mushroom_Burrito"):
                self.assertIn(node_id, self.dishes[dish]["nodes"])

    def test_no_assemble_nodes_left_on_demo3(self):
        self.assertEqual([n["id"] for n in self.recipes["nodes"] if n["op"] == "assemble"], [])

    def test_cook_steps_and_resources(self):
        for cook, step in (("cook:FriedChicken", "FryingPan"), ("cook:FriedBurritoMeat", "FryingPan"),
                           ("cook:FriedMushrooms", "FryingPan"), ("cook:BoiledRice", "Pot")):
            self.assertEqual(self.nodes[cook]["step"], step)
        self.assertEqual(self.nodes["cut:BurritoMeat"]["resource"], "board")
        self.assertEqual(self.nodes["pour:Mushroom"]["resource"], "counter")
        self.assertEqual(self.nodes["place:Tortilla"]["resource"], "counter")


if __name__ == "__main__":
    unittest.main()
