#!/usr/bin/env python3
'''Analyze an M2_OBJECT_TRACE CSV for the native-scene feasibility test.

Input columns: frame,pool,oba,obc,mode,matrix0..matrix11,vertices,source.
Geometry cache requirement: the object-data cache is viable only if
ROM objects carry >=50% of vertices before cull and a rigid relative-matrix
clique covers >=50% of the ROM vertices present in every frame.

A repeated ROM geometry key within one frame is indeterminate rather than a
refutation: geometry keys do not identify object instances, so rows are never
paired by ordinal or deduplicated by matrix.
'''
from __future__ import annotations

import argparse
import csv
import itertools
import json
import math
from collections import defaultdict
from pathlib import Path
from fractions import Fraction
from rigidity_coverage import best_coverage_clique, ExactSearchLimit

ANGLE_LIMIT = math.radians(0.05)
TRANSLATION_LIMIT = 0.01
# R^T R and det(R) must be this close to I and +1. Unsupported transforms are
# rejected below, never normalized or treated as identity.
ROTATION_TOLERANCE = 1e-6


def det3(m):
    a, b, c = m[0]
    d, e, f = m[1]
    g, h, i = m[2]
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)


def inv3(m):
    det = det3(m)
    if abs(det) < 1e-12:
        return None
    a, b, c = m[0]
    d, e, f = m[1]
    g, h, i = m[2]
    return [
        [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
        [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
        [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det],
    ]


def matmul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3)] for r in range(3)]


def matvec(a, v):
    return [sum(a[r][k] * v[k] for k in range(3)) for r in range(3)]


def transpose3(m):
    return [[m[c][r] for c in range(3)] for r in range(3)]


def is_finite_matrix(m):
    return m is not None and all(
        math.isfinite(m[r][c]) for r in range(3) for c in range(4)
    )


def is_proper_rotation(r):
    '''True only for a finite orthonormal matrix with determinant +1.'''
    if r is None:
        return False
    if any(not math.isfinite(r[i][j]) for i in range(3) for j in range(3)):
        return False
    for i in range(3):
        for j in range(3):
            dot = sum(r[k][i] * r[k][j] for k in range(3))
            target = 1.0 if i == j else 0.0
            if abs(dot - target) > ROTATION_TOLERANCE:
                return False
    return abs(det3(r) - 1.0) <= ROTATION_TOLERANCE


def relative(mi, mj):
    '''Return R = inv(Mi) * Mj for proper finite 3x4 rigid transforms.'''
    if not is_finite_matrix(mi) or not is_finite_matrix(mj):
        return None
    ri = [row[:3] for row in mi[:3]]
    ti = [mi[r][3] for r in range(3)]
    rj = [row[:3] for row in mj[:3]]
    tj = [mj[r][3] for r in range(3)]
    if not is_proper_rotation(ri) or not is_proper_rotation(rj):
        return None
    inv = inv3(ri)
    if inv is None:
        return None
    r = matmul(inv, rj)
    t = matvec(inv, [tj[k] - ti[k] for k in range(3)])
    if not is_proper_rotation(r):
        return None
    if not all(math.isfinite(x) for x in t):
        return None
    return r, t


def angle_of(r):
    '''Trace angle for a proper rotation; unsupported input is rejected.'''
    if not is_proper_rotation(r):
        return math.inf
    tr = r[0][0] + r[1][1] + r[2][2]
    return math.acos(max(-1.0, min(1.0, (tr - 1.0) / 2.0)))


def parse_matrix(row, prefix='matrix'):
    # geo.cpp writes its 12 floats unchanged: three basis columns, then
    # translation. Normalize to the row-major 3x4 form used by relative().
    # transform_point: x = m0*x + m3*y + m6*z + m9 (and similarly y/z).
    values = [float(row[f'{prefix}{i}']) for i in range(12)]
    return [[values[r], values[3 + r], values[6 + r], values[9 + r]] for r in range(3)]


def key_of(row):
    return (int(row['pool']), int(row['oba']), int(row['obc']), int(row['mode']))


def ambiguity_key_of(row):
    # obc is a link-prefix/LOD discriminator, not an instance identity. Rows
    # sharing pool/oba/mode may be distinct instances even when obc differs.
    return (int(row['pool']), int(row['oba']), int(row['mode']))


def best_clique(nodes, edges, weights):
    """Compatibility wrapper: exact single-frame objective."""
    return best_coverage_clique(nodes, edges, [weights])


def analyze(path: Path) -> dict:
    rows = list(csv.DictReader(path.open(encoding='utf-8')))
    if not rows:
        return {'verdict': 'no_data', 'rows': 0}
    frames = sorted({int(r['frame']) for r in rows})
    by_frame = defaultdict(list)
    for r in rows:
        by_frame[int(r['frame'])].append(r)
    source_vertices = defaultdict(int)
    for r in rows:
        source_vertices[r['source']] += int(r['vertices'])
    total = sum(source_vertices.values())
    rom_share = source_vertices.get('ROM', 0) / total if total else 0.0

    # Detect repeated ROM geometry keys before collapsing each frame into a dict.
    # Repeated keys, even with identical matrices, cannot be matched across
    # frames without inventing persistent instance identities.
    ambiguous_frames = set()
    repeated_groups = 0
    repeated_rows = 0
    for frame, frame_rows in by_frame.items():
        counts = defaultdict(int)
        for r in frame_rows:
            if r['source'] == 'ROM':
                counts[ambiguity_key_of(r)] += 1
        repeated = [n for n in counts.values() if n > 1]
        if repeated:
            ambiguous_frames.add(frame)
            repeated_groups += len(repeated)
            repeated_rows += sum(repeated)
    if ambiguous_frames:
        return {
            'rows': len(rows),
            'frames': len(frames),
            'source_vertices': dict(source_vertices),
            'rom_share': round(rom_share, 4),
            'ambiguous_frames': len(ambiguous_frames),
            'duplicate_rom_key_groups': repeated_groups,
            'duplicate_rom_rows': repeated_rows,
            'reason': (
                f'{repeated_groups} repeated ROM geometry key group(s) in '
                f'{len(ambiguous_frames)} frame(s) involve {repeated_rows} ROM row(s); '
                'geometry keys do not uniquely identify instances within a frame, '
                'so rows cannot be associated across frames without inventing '
                'persistent identities; the trace is indeterminate rather than a refutation'
            ),
            'verdict': 'alternative_indeterminee',
        }

    # Keys present with source ROM in every frame.
    rom_by_frame = {
        f: {key_of(r): r for r in rs if r['source'] == 'ROM'}
        for f, rs in by_frame.items()
    }
    common = set.intersection(*(set(m) for m in rom_by_frame.values())) if rom_by_frame else set()
    weights_by_frame = [{k: int(rom_by_frame[f][k]['vertices']) for k in common}
                        for f in frames]
    matrices = {k: [parse_matrix(rom_by_frame[f][k]) for f in frames] for k in common}
    edges = {k: set() for k in common}
    optimistic = {k: set() for k in common}
    pairs = rigid_pairs = unsupported_pairs = 0
    for a, b in itertools.combinations(sorted(common), 2):
        pairs += 1
        samples = [relative(ma, mb) for ma, mb in zip(matrices[a], matrices[b])]
        supported = [sample for sample in samples if sample is not None]
        unknown = len(supported) != len(samples) or len(frames) < 2
        if len(supported) != len(samples):
            unsupported_pairs += 1
        # Any two supported samples can refute a pair, even if another sample
        # is unsupported. Unknown evidence must not erase an actual mismatch.
        different = False
        if supported:
            base = supported[0]
            for rel in supported[1:]:
                dr = matmul(rel[0], transpose3(base[0]))
                dt = [rel[1][k] - base[1][k] for k in range(3)]
                if angle_of(dr) > ANGLE_LIMIT or math.sqrt(sum(x*x for x in dt)) > TRANSLATION_LIMIT:
                    different = True
                    break
        if not different:
            optimistic[a].add(b)
            optimistic[b].add(a)
            if not unknown:
                rigid_pairs += 1
                edges[a].add(b)
                edges[b].add(a)

    def coverage(clique):
        return [Fraction(sum(w[k] for k in clique), sum(w.values()))
                if sum(w.values()) else Fraction(0) for w in weights_by_frame]

    search_limit = None
    try:
        clique = best_coverage_clique(common, edges, weights_by_frame)
        possible = best_coverage_clique(common, optimistic, weights_by_frame)
    except ExactSearchLimit as exc:
        # Bounded exact search must never silently become a heuristic verdict.
        search_limit = str(exc)
        clique, possible = [], []
    covers = coverage(clique)
    possible_covers = coverage(possible)
    clique_cover = min(covers)
    possible_cover = min(possible_covers)
    valid = (not search_limit and len(frames) >= 2 and len(clique) >= 2
             and rom_share >= 0.5 and clique_cover >= Fraction(1, 2))
    impossible = (rom_share < 0.5 or (not search_limit and len(frames) >= 2
                  and (len(possible) < 2 or possible_cover < Fraction(1, 2))))
    verdict = ('alternative_valide' if valid else 'alternative_refutee' if impossible
               else 'alternative_indeterminee')
    return {
        'rows': len(rows), 'frames': len(frames),
        'source_vertices': dict(source_vertices), 'rom_share': round(rom_share, 4),
        'common_rom_keys': len(common), 'rigid_pairs': rigid_pairs,
        'unsupported_pairs': unsupported_pairs, 'pairs': pairs,
        'clique_size': len(clique), 'clique_cover': round(float(clique_cover), 4),
        'clique_cover_by_frame': [round(float(c), 4) for c in covers],
        'optimistic_clique_size': len(possible),
        'optimistic_clique_cover': round(float(possible_cover), 4),
        'search_limit': search_limit,
        'verdict': verdict,
        'reason': (search_limit or 'insufficient temporal samples or unsupported transforms may change the verdict'
                   if verdict == 'alternative_indeterminee' else None),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('csv', type=Path)
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    result = analyze(a.csv)
    print(json.dumps(result, indent=2, ensure_ascii=False) if a.json else json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
