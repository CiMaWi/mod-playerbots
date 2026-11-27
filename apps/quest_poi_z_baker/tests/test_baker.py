"""Pure-function tests for the quest POI Z baker (no database or mmaps)."""

from __future__ import annotations

import math
import sys
import unittest
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_DIR))

from quest_poi_z_baker import (  # noqa: E402
    bake_point, box_floors, cluster_levels, dist2d, level_floor, poly_floor, spawns_in_radius, walkable,
)


class ClusterLevelsTests(unittest.TestCase):
    def test_empty(self) -> None:
        self.assertEqual(cluster_levels([], 8.0), [])

    def test_splits_at_gap(self) -> None:
        clusters = cluster_levels([(1.0, "a"), (2.0, "b"), (20.0, "c")], 8.0)
        self.assertEqual(len(clusters), 2)
        self.assertAlmostEqual(clusters[0]["zmean"], 1.5)
        self.assertEqual(clusters[0]["items"], ["a", "b"])
        self.assertAlmostEqual(clusters[1]["zmean"], 20.0)

    def test_keeps_single_cluster(self) -> None:
        clusters = cluster_levels([(1.0, "a"), (4.0, "b"), (7.0, "c")], 8.0)
        self.assertEqual(len(clusters), 1)
        self.assertAlmostEqual(clusters[0]["zmean"], 4.0)


class SpawnRadiusTests(unittest.TestCase):
    def test_filters_by_radius(self) -> None:
        spawns = [(0.0, 0.0, 10.0, 1), (5.0, 0.0, 11.0, 2), (20.0, 0.0, 12.0, 3)]
        xs = [s[0] for s in spawns]
        got = spawns_in_radius(xs, spawns, 1.0, 0.0, 6.0)
        self.assertEqual([s[3] for s in got], [1, 2])

    def test_empty(self) -> None:
        self.assertEqual(spawns_in_radius([], [], 0.0, 0.0, 10.0), [])

    def test_dist2d(self) -> None:
        self.assertAlmostEqual(dist2d(0.0, 0.0, 3.0, 4.0), 5.0)


class BakePointTests(unittest.TestCase):
    def test_empty(self) -> None:
        self.assertEqual(bake_point(0.0, 0.0, [], [], [], [], []), [])

    def test_ranks_trigger_and_quest_spawn_first(self) -> None:
        near_polys = [
            (0.0, 0.0, 10.0, []),
            (1.0, 0.0, 10.5, []),
            (0.0, 0.0, 30.0, []),
        ]
        candidates = bake_point(
            0.0, 0.0, near_polys, truth_zs=[10.0], trigger_zs=[10.0],
            cre_near=[(0.0, 0.0, 10.0, 123)], go_near=[],
        )
        self.assertTrue(candidates)
        best = candidates[0]
        self.assertGreaterEqual(best["confidence"], 90)
        self.assertTrue(best["quest_spawn"])
        self.assertTrue(best["trigger"])
        self.assertTrue(best["nav_ok"])
        self.assertIn("quest-spawn", best["evidence"])
        self.assertIn("trigger", best["evidence"])
        self.assertIn("navmesh=", best["evidence"])
        self.assertTrue(best["walkable"])
        self.assertLessEqual(len(best["walkable"]), 6)
        self.assertLessEqual(len(candidates), 4)

    def test_navmesh_only_is_lower_confidence(self) -> None:
        candidates = bake_point(
            0.0, 0.0, [(0.0, 0.0, 42.0, [])], truth_zs=[], trigger_zs=[],
            cre_near=[], go_near=[],
        )
        self.assertEqual(len(candidates), 1)
        self.assertTrue(candidates[0]["nav_ok"])
        self.assertGreaterEqual(candidates[0]["confidence"], 45)
        self.assertLess(candidates[0]["confidence"], 90)
        self.assertIn("navmesh=1", candidates[0]["evidence"])

    def test_quest_spawn_requires_matching_spawn(self) -> None:
        candidates = bake_point(
            0.0, 0.0, [], truth_zs=[0.0], trigger_zs=[],
            cre_near=[(0.0, 0.0, 0.0, 123), (0.0, 0.0, 30.0, 456)], go_near=[],
        )
        by_z = {c["z"]: c for c in candidates}
        self.assertTrue(by_z[0.0]["quest_spawn"])
        self.assertFalse(by_z[30.0]["quest_spawn"])
        self.assertEqual(by_z[30.0]["confidence"], 30)


def _square(cx: float, cy: float, z: float, half: float = 2.0) -> tuple:
    wpts = [(cx - half, cy - half, z), (cx + half, cy - half, z), (cx + half, cy + half, z), (cx - half, cy + half, z)]
    return (cx, cy, z, wpts)


class LevelFloorTests(unittest.TestCase):
    """Pathing PA59c: the emitted z is the level's floor at the point, not the middle of its polygons' heights."""

    def test_slope_takes_the_floor_under_the_point(self) -> None:
        # the shape of quest 2118 poi 1 point 6: the one polygon under the point at 44, the level's other polygons
        # up a slope; the level's height (52) lies 8 yd above the floor, beyond the engine's 7 yd
        near_polys = [_square(0.0, 0.0, 44.0)] + [_square(8.0 * i, 0.0, 44.0 + 4.0 * i) for i in range(1, 5)]
        candidates = bake_point(0.0, 0.0, near_polys, [], [], [], [])
        self.assertEqual(len(candidates), 1)
        self.assertAlmostEqual(candidates[0]["level"], 52.0)
        self.assertAlmostEqual(candidates[0]["z"], 44.0)
        self.assertEqual(candidates[0]["walkable"][0], (0.0, 0.0, 44.0))

    def test_inside_a_tilted_polygon_takes_its_plane(self) -> None:
        tri = {"cx": 0.0, "cy": 0.0, "cz": 0.0, "wpts": [(-4.0, -4.0, 0.0), (4.0, -4.0, 8.0), (0.0, 4.0, 4.0)]}
        d, z = poly_floor(0.0, 0.0, tri)
        self.assertEqual(d, 0.0)
        self.assertAlmostEqual(z, 4.0)

    def test_off_every_polygon_takes_the_nearest_edge(self) -> None:
        tri = {"cx": 7.0, "cy": 0.0, "cz": 12.0, "wpts": [(5.0, -2.0, 10.0), (9.0, 0.0, 14.0), (5.0, 2.0, 10.0)]}
        d, z = poly_floor(0.0, 0.0, tri)
        self.assertAlmostEqual(d, 5.0)
        self.assertAlmostEqual(z, 10.0)

    def test_the_box_is_the_engines(self) -> None:
        # bounds overlapping the 3 yd square count even when the polygon's nearest point is 3.6 yd away; a polygon
        # whose bounds miss the square lies beside it; steep ground and a floor beyond 6.5 yd do not count
        diagonal = (6.0, 6.0, 20.0, [(3.0, 2.0, 20.0), (9.0, 2.0, 20.0), (2.0, 9.0, 20.0)])
        off = (7.0, 0.0, 12.0, [(5.0, -2.0, 10.0), (9.0, 0.0, 14.0), (5.0, 2.0, 10.0)])
        steep = _square(0.0, 0.0, 7.0, 1.0) + (0x10,)
        thin = (6.0, 3.0, 9.0, [(2.9, 9.0, 9.0), (9.0, 2.9, 9.0), (9.0, 9.0, 9.0)])
        inside, beside = box_floors(0.0, 0.0, [diagonal, off, steep, thin])
        self.assertEqual(len(inside), 1)
        self.assertAlmostEqual(inside[0][0], math.sqrt(13.0))
        self.assertEqual(inside[0][1:], (20.0, id(diagonal)))
        self.assertEqual([(round(f[0], 2), round(f[1], 2)) for f in beside], [(5.0, 10.0)])
        # an empty box takes the floor beside it; a level with neither keeps its height
        near = bake_point(0.0, 0.0, [off], [], [], [], [])
        self.assertAlmostEqual(near[0]["z"], 10.0)
        only = bake_point(0.0, 0.0, [thin], [], [], [], [])
        self.assertAlmostEqual(only[0]["z"], only[0]["level"])

    def test_two_floors_keep_their_own_polygons(self) -> None:
        # a building: ground and upper floor over the point stay two ranks, each on its own floor
        near_polys = [_square(0.0, 0.0, 10.0), _square(5.0, 0.0, 11.0), _square(0.0, 0.0, 30.0)]
        candidates = bake_point(0.0, 0.0, near_polys, [], [], [], [])
        self.assertEqual(sorted(c["z"] for c in candidates), [10.0, 30.0])

    def test_only_polygons_the_bots_walk_hold_the_floor(self) -> None:
        # steep ground (0x10) under the point, walkable ground 2 yd beside it: the floor is the ground's
        steep = _square(0.0, 0.0, 20.0) + (0x10,)
        ground = _square(4.0, 0.0, 16.0) + (0x01,)
        candidates = bake_point(0.0, 0.0, [steep, ground], [], [], [], [])
        self.assertEqual(len(candidates), 1)
        self.assertAlmostEqual(candidates[0]["z"], 16.0)
        self.assertTrue(walkable(0x01) and walkable(0x08) and walkable(0x09))
        self.assertFalse(walkable(0x11) or walkable(0x04) or walkable(0x02) or walkable(0))
        # a level of steep ground only keeps its height
        only = bake_point(0.0, 0.0, [steep, _square(6.0, 0.0, 24.0) + (0x10,)], [], [], [], [])
        self.assertAlmostEqual(only[0]["z"], only[0]["level"])

    def test_own_floor_first_then_the_box_floor_nearest_the_level(self) -> None:
        # the floor under the point (154) belongs to neither level's 8 yd band (165, 142): both take it
        box = [(0.0, 153.75, 1)]
        self.assertEqual(level_floor(165.0, {7}, box), 153.75)
        self.assertEqual(level_floor(142.0, set(), box), 153.75)
        # the level's own polygon is taken by 2D distance, before a nearer height of another level's
        mixed = [(4.6, 262.0, 1), (0.0, 406.0, 2), (5.0, 258.0, 3)]
        self.assertEqual(level_floor(259.0, {1, 3}, mixed), 262.0)
        self.assertEqual(level_floor(259.0, set(), mixed), 258.0)
        self.assertEqual(level_floor(42.0, {1}, []), 42.0)
        # the big polygon under the point whose centroid lies beyond the scan radius still holds the floor
        big = (0.0, 50.0, 30.0, [(-60.0, -60.0, 30.0), (60.0, -60.0, 30.0), (60.0, 60.0, 30.0), (-60.0, 60.0, 30.0)])
        slope = [_square(8.0 * i, 0.0, 38.0 + 4.0 * i) for i in range(1, 4)]
        got = bake_point(0.0, 0.0, slope, [], [], [], [], box_polys=slope + [big])
        self.assertAlmostEqual(got[0]["z"], 30.0)

    def test_spawn_only_level_keeps_its_height(self) -> None:
        spawn_only = bake_point(0.0, 0.0, [_square(0.0, 0.0, 10.0)], [], [], [(0.0, 0.0, 40.0, 7)], [])
        self.assertEqual(sorted(c["z"] for c in spawn_only), [10.0, 40.0])

    def test_without_vertices_the_centroid_is_the_polygon(self) -> None:
        d, z = poly_floor(3.0, 4.0, {"cx": 0.0, "cy": 0.0, "cz": 7.0})
        self.assertAlmostEqual(d, 5.0)
        self.assertEqual(z, 7.0)

if __name__ == "__main__":
    unittest.main()
