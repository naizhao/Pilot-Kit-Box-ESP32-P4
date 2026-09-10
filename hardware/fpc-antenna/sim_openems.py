#!/usr/bin/env python3
"""openEMS FDTD re-verification entry for REV-A3.1-fixed.

Run inside container:
  docker run --rm -v "$PWD":/work -w /work --entrypoint python3 oems:local sim_openems.py

The copper topology comes from antenna_geometry.py: two mirrored three-fold
arms, top-edge SIG/GND tongues, and a lumped 50 ohm port across their 1.2 mm
horizontal gap. The physical source board uses 12 um copper in a 0.11 mm FPC
stackup. For grid alignment openEMS models copper as a 0.1 mm PEC cell and PI
as a homogeneous 0.11 mm slab (er=3.2, tanD~=0.02); coverlay and adhesives are
not split out. These are simulation approximations, not a measured stackup.
"""
import json
import os
import time

from antenna_geometry import PIECE_NAMES, geometry_for

T_CU = 0.1            # grid-aligned PEC cell; physical copper is 0.012 mm
Z_CU0, Z_CU1 = 0.0, T_CU
T_SUB = 0.11          # homogeneous slab; physical total FPC thickness
Z_SUB0, Z_SUB1 = -T_SUB, 0.0

F0, FC = 1.05e9, 0.45e9
F_MIN, F_MAX, NF = 0.7e9, 1.3e9, 601


def simulation_geometry(name):
    """Return the pure-Python geometry actually consumed by openEMS."""
    return geometry_for(name)


def _local_point(geometry, point):
    x, y = point
    return x - 50.0, y - (geometry.top + 8.0)


def _segment_box(geometry, segment):
    (x1, y1), (x2, y2) = tuple(_local_point(geometry, point) for point in segment)
    half_width = geometry.trace_width / 2.0
    if abs(y1 - y2) < 1e-12:
        return min(x1, x2), y1 - half_width, max(x1, x2), y1 + half_width
    if abs(x1 - x2) < 1e-12:
        return x1 - half_width, min(y1, y2), x1 + half_width, max(y1, y2)
    raise ValueError("Frozen antenna segments must be axis-aligned: %r" % (segment,))


def _rectangle_box(geometry, rectangle):
    (x1, y1), (x2, y2) = tuple(_local_point(geometry, point) for point in rectangle)
    return min(x1, x2), min(y1, y2), max(x1, x2), max(y1, y2)


def copper_boxes(name):
    """Return all grid-aligned copper boxes before adding their z extent."""
    geometry = simulation_geometry(name)
    arm_boxes = tuple(_segment_box(geometry, segment)
                      for segment in geometry.left_arm + geometry.right_arm)
    tongue_boxes = tuple(_rectangle_box(geometry, rectangle)
                         for rectangle in (geometry.sig_tongue, geometry.gnd_tongue))
    return arm_boxes + tongue_boxes


def sim_variant(name, outdir, nthreads):
    # Solver imports stay at the modeling boundary; pure geometry needs none.
    import numpy as np
    from CSXCAD import ContinuousStructure
    from CSXCAD.SmoothMeshLines import SmoothMeshLines
    from openEMS import openEMS

    geometry = simulation_geometry(name)
    boxes = copper_boxes(name)
    csx = ContinuousStructure()
    metal = csx.AddMetal("copper")
    for x1, y1, x2, y2 in boxes:
        metal.AddBox([x1, y1, Z_CU0], [x2, y2, Z_CU1])

    # kappa~=0.0036 S/m corresponds to tanD~=0.02 near 1 GHz for er=3.2.
    pi = csx.AddMaterial("PI_approx", epsilon=3.2, kappa=0.0036)
    pi.AddBox([-45.0, -8.0, Z_SUB0], [45.0, 8.0, Z_SUB1])

    fdtd = openEMS()
    fdtd.SetCSX(csx)
    fdtd.SetBoundaryCond(["MUR"] * 6)
    fdtd.SetNumberOfTimeSteps(220000)
    fdtd.SetMaxTime(30e-9)
    fdtd.SetGaussExcite(F0, FC)
    fdtd.SetEndCriteria(1e-3)

    grid = csx.GetGrid()
    grid.SetDeltaUnit(1e-3)
    x_features = {-80.0, -45.0, 45.0, 80.0}
    y_features = {-60.0, -8.0, 8.0, 60.0}
    for x1, y1, x2, y2 in boxes:
        x_features.update((x1, x2))
        y_features.update((y1, y2))
    gap = _rectangle_box(geometry, geometry.feed_gap)
    x_features.update((gap[0], gap[2]))
    y_features.update((gap[1], gap[3]))
    xlines = SmoothMeshLines(np.array(sorted(x_features)), 0.8, 1.4)
    ylines = SmoothMeshLines(np.array(sorted(y_features)), 0.8, 1.4)
    zlines = np.unique(np.concatenate([
        np.array([Z_SUB0, Z_SUB1, Z_CU1]),
        SmoothMeshLines(np.array([-50.0, -0.3, Z_SUB0]), 6.0, 1.5),
        SmoothMeshLines(np.array([Z_CU1, 0.4, 50.0]), 6.0, 1.5),
    ]))
    grid.AddLine("x", xlines)
    grid.AddLine("y", ylines)
    grid.AddLine("z", np.sort(zlines))

    # Shared gap x endpoints are 50.4/51.6 panel coordinates (0.4/1.6 local).
    port = fdtd.AddLumpedPort(
        0, 50.0, [gap[0], gap[1], Z_CU0], [gap[2], gap[3], Z_CU1],
        "x", excite=True,
    )

    sim_path = os.path.join("/tmp", f"sim_{name.replace('-', 'm')}")
    os.makedirs(sim_path, exist_ok=True)
    t0 = time.time()
    fdtd.Run(sim_path, verbose=-1, numThreads=nthreads)
    t_run = time.time() - t0

    f = np.linspace(F_MIN, F_MAX, NF)
    port.CalcPort(sim_path, f)
    uf = np.asarray(port.uf_tot)
    ui = np.asarray(port.if_tot)
    Z = uf / np.where(np.abs(ui) < 1e-15, 1e-15, ui)
    s11db = 20 * np.log10(np.maximum(np.abs((Z - 50) / (Z + 50)), 1e-12))

    i = int(np.argmin(s11db))
    fmin = float(f[i] / 1e6)
    res = {
        "variant": name,
        "f_min_MHz": fmin,
        "S11_min_dB": float(s11db[i]),
        "S11_at_1090_dB": float(s11db[np.searchsorted(f, 1090e6)]),
        "S11_at_978_dB": float(s11db[np.searchsorted(f, 978e6)]),
        "Z_1090": [float(Z[np.searchsorted(f, 1090e6)].real),
                   float(Z[np.searchsorted(f, 1090e6)].imag)],
        "run_s": round(t_run, 1),
    }
    os.makedirs(outdir, exist_ok=True)
    np.savetxt(os.path.join(outdir, f"{name.replace('-', 'm')}_s11.csv"),
               np.column_stack([f / 1e6, s11db, Z.real, Z.imag]),
               delimiter=",", header="freq_MHz,S11_dB,ReZ,ImZ", comments="")
    print(json.dumps(res), flush=True)
    return res


def main():
    outdir = "/work/sim_out"
    nthreads = min(12, os.cpu_count() or 4)
    variants = os.environ.get("VARIANTS", ",".join(PIECE_NAMES)).split(",")
    results = []
    for v in variants:
        if v not in PIECE_NAMES:
            print("skip", v)
            continue
        results.append(sim_variant(v, outdir, nthreads))
        with open(os.path.join(outdir, "summary.json"), "w") as fp:
            json.dump(results, fp, indent=1)
    print("ALL DONE")


if __name__ == "__main__":
    main()
