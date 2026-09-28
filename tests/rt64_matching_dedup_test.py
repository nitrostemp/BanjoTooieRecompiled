#!/usr/bin/env python3
"""Check exact set-union equivalence of the RT64 call-work deduplication."""

from __future__ import annotations

import dataclasses
import itertools
import random
import unittest


TEXTURE_GEN_MASK = 0x00020000 | 0x00040000


@dataclasses.dataclass(frozen=True)
class Call:
    call_hash: int
    scene_projection: int
    min_world: int
    max_world: int
    tile_index: int
    tile_count: int
    geometry_mode: int
    face_start: int
    transform_matching: bool
    tile_interpolation: bool
    tile_matching: bool

@dataclasses.dataclass
class Tables:
    matrix_ids: dict[tuple[int, int], int]
    matrix_order_auto: dict[tuple[int, int], bool]
    tile_ids: dict[tuple[int, int], int]
    look_at: dict[tuple[int, int], int]


def add_transform_effects(cur: Call, prev: Call, tables: Tables,
                          transforms: set[tuple[int, int]]) -> None:
    cur_count = cur.max_world - cur.min_world + 1
    prev_count = prev.max_world - prev.min_world + 1
    if cur_count == prev_count and cur.transform_matching and prev.transform_matching:
        for offset in range(cur_count):
            cur_index = cur.min_world + offset
            prev_index = prev.min_world + offset
            cur_key = (cur.scene_projection, cur_index)
            prev_key = (prev.scene_projection, prev_index)
            matrix_id = tables.matrix_ids[cur_key]
            if (matrix_id == tables.matrix_ids[prev_key] and
                    (matrix_id == 0xFFFFFFFF or
                     (matrix_id != 0 and tables.matrix_order_auto[cur_key]))):
                # RT64 intentionally stores only the numeric indices here.
                transforms.add((cur_index, prev_index))

def add_tile_effects(cur: Call, prev: Call, tables: Tables,
                     tiles: set[tuple[int, int]]) -> None:
    if cur.tile_count == prev.tile_count:
        for offset in range(cur.tile_count):
            cur_index = cur.tile_index + offset
            prev_index = prev.tile_index + offset
            if (cur.tile_matching and prev.tile_matching and
                    tables.tile_ids[(cur.scene_projection, cur_index)] !=
                    tables.tile_ids[(prev.scene_projection, prev_index)]):
                continue
            tiles.add((cur_index, prev_index))

def collect(cur_calls: list[Call], prev_calls: list[Call], tables: Tables,
            deduplicate: bool) -> tuple[tuple[frozenset[tuple[int, int]], ...], dict[str, int]]:
    transforms: set[tuple[int, int]] = set()
    tiles: set[tuple[int, int]] = set()
    look_at: set[tuple[int, int]] = set()
    examined = {"call": 0, "transform": 0, "tile": 0, "look_at": 0}
    hashes = sorted({call.call_hash for call in cur_calls})
    for call_hash in hashes:
        cur_bucket = [call for call in cur_calls if call.call_hash == call_hash]
        prev_bucket = [call for call in prev_calls if call.call_hash == call_hash]
        if not deduplicate:
            for cur, prev in itertools.product(cur_bucket, prev_bucket):
                examined["call"] += 1
                if cur.transform_matching and prev.transform_matching:
                    examined["transform"] += 1
                    add_transform_effects(cur, prev, tables, transforms)
                if cur.tile_interpolation and prev.tile_interpolation:
                    examined["tile"] += 1
                    add_tile_effects(cur, prev, tables, tiles)
                if ((cur.geometry_mode & TEXTURE_GEN_MASK) == TEXTURE_GEN_MASK and
                        (prev.geometry_mode & TEXTURE_GEN_MASK) == TEXTURE_GEN_MASK):
                    examined["look_at"] += 1
                    look_at.add((tables.look_at[(cur.scene_projection, cur.face_start)],
                                 tables.look_at[(prev.scene_projection, prev.face_start)]))
            continue

        cur_transforms = {(c.scene_projection, c.min_world, c.max_world): c
                          for c in cur_bucket if c.transform_matching}
        prev_transforms = {(c.scene_projection, c.min_world, c.max_world): c
                           for c in prev_bucket if c.transform_matching}
        for cur, prev in itertools.product(cur_transforms.values(), prev_transforms.values()):
            examined["transform"] += 1
            add_transform_effects(cur, prev, tables, transforms)

        cur_tiles = {(c.scene_projection, c.tile_index, c.tile_count, c.tile_matching): c
                     for c in cur_bucket if c.tile_interpolation}
        prev_tiles = {(c.scene_projection, c.tile_index, c.tile_count, c.tile_matching): c
                      for c in prev_bucket if c.tile_interpolation}
        for cur, prev in itertools.product(cur_tiles.values(), prev_tiles.values()):
            examined["tile"] += 1
            add_tile_effects(cur, prev, tables, tiles)

        cur_look_at = {tables.look_at[(c.scene_projection, c.face_start)] for c in cur_bucket
                       if (c.geometry_mode & TEXTURE_GEN_MASK) == TEXTURE_GEN_MASK}
        prev_look_at = {tables.look_at[(c.scene_projection, c.face_start)] for c in prev_bucket
                        if (c.geometry_mode & TEXTURE_GEN_MASK) == TEXTURE_GEN_MASK}
        for cur_index, prev_index in itertools.product(cur_look_at, prev_look_at):
            examined["look_at"] += 1
            look_at.add((cur_index, prev_index))
    return (frozenset(transforms), frozenset(tiles), frozenset(look_at)), examined


def make_tables() -> Tables:
    matrix_ids = {}
    matrix_order_auto = {}
    tile_ids = {}
    look_at = {}
    for projection in range(4):
        for index in range(32):
            matrix_ids[(projection, index)] = 0xFFFFFFFF if index % 3 else index
            matrix_order_auto[(projection, index)] = index % 2 == 0
            tile_ids[(projection, index)] = (projection + index) % 5
            look_at[(projection, index)] = (projection * 7 + index) % 11
    return Tables(matrix_ids, matrix_order_auto, tile_ids, look_at)


class MatchingDedupTest(unittest.TestCase):
    def test_repeated_calls_preserve_union_and_reduce_cross_product(self) -> None:
        call = Call(7, 0, 1, 2, 3, 2, TEXTURE_GEN_MASK, 4, True, True, True)
        baseline, baseline_pairs = collect([call] * 128, [call] * 128, make_tables(), False)
        optimized, optimized_pairs = collect([call] * 128, [call] * 128, make_tables(), True)
        self.assertEqual(optimized, baseline)
        self.assertEqual(baseline_pairs["call"], 16384)
        self.assertEqual(optimized_pairs["transform"], 1)
        self.assertEqual(optimized_pairs["tile"], 1)
        self.assertEqual(optimized_pairs["look_at"], 1)

    def test_hash_collisions_flags_and_projection_identity_preserve_union(self) -> None:
        calls = [
            Call(9, 0, 1, 2, 3, 2, TEXTURE_GEN_MASK, 4, True, True, True),
            Call(9, 0, 1, 2, 3, 2, TEXTURE_GEN_MASK, 4, True, True, True),
            Call(9, 1, 1, 2, 3, 2, TEXTURE_GEN_MASK, 4, True, True, False),
            Call(9, 1, 5, 5, 8, 1, 0, 7, False, True, False),
            Call(12, 0, 6, 7, 9, 0, 0, 2, True, False, False),
        ]
        prev = list(reversed(calls)) + [calls[0]]
        baseline, baseline_pairs = collect(calls, prev, make_tables(), False)
        optimized, optimized_pairs = collect(calls, prev, make_tables(), True)
        self.assertEqual(optimized, baseline)
        self.assertLess(sum(optimized_pairs.values()), sum(baseline_pairs.values()))

    def test_legacy_numeric_pair_alias_across_workloads_is_unchanged(self) -> None:
        # scene_projection stands in for the projection's workload identity.
        # RT64's existing sets intentionally retain only numeric indices after
        # collection; this candidate must neither merge inputs early nor repair
        # that downstream first-workload aliasing here.
        cur = [
            Call(5, 0, 1, 1, 3, 0, 0, 0, True, False, False),
            Call(5, 1, 1, 1, 3, 0, 0, 0, True, False, False),
        ]
        prev = [
            Call(5, 2, 1, 1, 3, 0, 0, 0, True, False, False),
            Call(5, 3, 1, 1, 3, 0, 0, 0, True, False, False),
        ]
        baseline, _ = collect(cur, prev, make_tables(), False)
        optimized, _ = collect(cur, prev, make_tables(), True)
        self.assertEqual(optimized, baseline)
        self.assertEqual(baseline[0], frozenset({(1, 1)}))

    def test_deterministic_generated_cases_preserve_union(self) -> None:
        rng = random.Random(0xB4A2)
        tables = make_tables()
        for _ in range(100):
            calls = []
            for _ in range(30):
                minimum = rng.randrange(0, 12)
                tile_index = rng.randrange(0, 12)
                calls.append(Call(
                    rng.randrange(0, 4), rng.randrange(0, 4), minimum,
                    minimum + rng.randrange(0, 3), tile_index, rng.randrange(0, 3),
                    TEXTURE_GEN_MASK if rng.randrange(0, 2) else 0,
                    rng.randrange(0, 12), bool(rng.randrange(0, 2)),
                    bool(rng.randrange(0, 2)), bool(rng.randrange(0, 2))))
            calls.extend(calls[:10])
            prev = calls[5:] + calls[:5]
            baseline, _ = collect(calls, prev, tables, False)
            optimized, _ = collect(calls, prev, tables, True)
            self.assertEqual(optimized, baseline)


if __name__ == "__main__":
    unittest.main()
