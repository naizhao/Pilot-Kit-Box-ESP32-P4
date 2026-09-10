# -*- coding: utf-8 -*-
"""
ADS-B FPC antenna - Ansys Electronics Desktop (HFSS) cross-check script.

Usage on the Windows HFSS machine (PyAEDT installed, e.g. AEDT 2023R2+):
    python hfss_adsb_fpc.py            # builds all 4 variants, one project each

What it builds per variant:
  - both mirrored three-fold arms and both top-edge solder tongues
  - 2D PEC sheet copper on a 0.11mm homogeneous PI approximation
    (er=3.2, tanD=0.02; coverlay/adhesives are not split out)
  - center-fed dipole, LUMPED_PORT 50ohm across the 1.2mm horizontal feed gap
  - radiation box 1/4 wavelength @ low band, PML-free radiation boundary
  - discrete sweep 0.8-1.3 GHz, S11 + Z11 reports, CSV export next to project

The shared geometry is REV-A3.1-fixed in antenna_geometry.py. The source FPC
uses 0.012mm copper; the zero-thickness PEC sheet is a solver approximation,
not a claim about the fabrication stackup.
"""
import os

from antenna_geometry import PIECE_NAMES, geometry_for

UNIT = "mm"
SUB_T = 0.11

F_LIMITS_GHZ = (0.8, 1.3)
SETUP_NAME = "Sweep1090"
SWEEP_NAME = "sw0800_1300"


def simulation_geometry(name):
    """Return the pure-Python geometry actually consumed by HFSS."""
    return geometry_for(name)


def build_variant(name, out_dir):
    # PyAEDT is required only when a caller actually builds/runs a model.
    try:
        from ansys.aedt.core import Hfss
    except ImportError:
        from pyaedt import Hfss
        current_pyaedt = False
    else:
        current_pyaedt = True

    geometry = simulation_geometry(name)
    proj = os.path.join(out_dir, f"adsb_fpc_{name.replace('-', 'm')}.aedt")
    if current_pyaedt:
        hfss = Hfss(
            project=proj, solution_type="Modal", design=name,
            new_desktop=False,
        )
    else:
        hfss = Hfss(
            projectname=proj, solution_type="Modal", designname=name,
            new_desktop_session=False,
        )
    hfss.modeler.model_units = UNIT

    # Homogeneous material values are modeling assumptions, not vendor data.
    pi_approx = hfss.materials.add_material("PI_approx")
    pi_approx.permittivity = 3.2
    pi_approx.dielectric_loss_tangent = 0.02
    hfss.modeler.create_box(
        origin=["5", str(geometry.top), "-%f" % SUB_T],
        sizes=["90", "16", "%f" % SUB_T],
        name="PI", material="PI_approx",
    )

    # Unite each arm only with its own tongue. The two feed conductors must
    # remain separate so the port can excite the 1.2 mm gap between them.
    copper_names = []
    segment_index = 0
    for side, segments, tongue in (
            ("sig", geometry.left_arm, geometry.sig_tongue),
            ("gnd", geometry.right_arm, geometry.gnd_tongue)):
        side_names = []
        for (x1, y1), (x2, y2) in segments:
            if abs(y1 - y2) < 1e-9:
                obj = hfss.modeler.create_rectangle(
                    orientation="XY",
                    origin=[str(min(x1, x2)),
                            str(y1 - geometry.trace_width / 2.0), "0"],
                    sizes=[str(abs(x2 - x1)), str(geometry.trace_width)],
                    name=f"cu_{name}_{segment_index}",
                )
            elif abs(x1 - x2) < 1e-9:
                obj = hfss.modeler.create_rectangle(
                    orientation="XY",
                    origin=[str(x1 - geometry.trace_width / 2.0),
                            str(min(y1, y2)), "0"],
                    sizes=[str(geometry.trace_width), str(abs(y2 - y1))],
                    name=f"cu_{name}_{segment_index}",
                )
            else:
                raise ValueError("Frozen antenna segments must be axis-aligned")
            side_names.append(obj.name)
            segment_index += 1
        (x1, y1), (x2, y2) = tongue
        tongue_sheet = hfss.modeler.create_rectangle(
            orientation="XY",
            origin=[str(x1), str(y1), "0"],
            sizes=[str(x2 - x1), str(y2 - y1)],
            name=f"tongue_{name}_{side}",
        )
        side_names.append(tongue_sheet.name)
        united = hfss.modeler.unite(side_names)
        if not united:
            raise RuntimeError("HFSS could not unite the %s arm sheets" % side)
        copper_names.append(united if isinstance(united, str) else side_names[0])

    for copper in copper_names:
        hfss.assign_material(copper, "copper")

    # Zero-thickness PEC sheets approximate the physical 0.012 mm copper.
    hfss.assign_perfecte_to_sheets(copper_names, name="PEC_cu")

    # Lumped port spans exactly the shared x=50.4..51.6 feed gap.
    (gap_x1, gap_y1), (gap_x2, gap_y2) = geometry.feed_gap
    port_sheet = hfss.modeler.create_rectangle(
        orientation="XY",
        origin=[str(gap_x1), str(gap_y1), "0"],
        sizes=[str(gap_x2 - gap_x1), str(gap_y2 - gap_y1)],
        name=f"port_{name}",
    )
    requested_port_name = f"P_{name.replace('-', 'm')}"
    axis_directions = getattr(hfss, "axis_directions", None)
    if axis_directions is None:
        axis_directions = hfss.AxisDir
    port = hfss.lumped_port(
        port_sheet.name, integration_line=axis_directions.XNeg,
        impedance=50, name=requested_port_name,
    )
    if not port:
        raise RuntimeError("HFSS port creation failed for %s" % name)
    port_name = port.name

    # radiation boundary: quarter wavelength at 800 MHz is 93.75mm -> use 100mm padding
    center_y = geometry.top + 8.0
    rb = hfss.modeler.create_box(
        origin=["-110", str(center_y - 120.0), "-120"], sizes=["320", "240", "240"],
        name="air",
    )
    hfss.modeler["air"].material_name = "air"
    hfss.assign_radiation_boundary_to_objects("air")

    # setup + sweep
    setup = hfss.create_setup(name=SETUP_NAME)
    setup.props["Frequency"] = "1.09GHz"
    setup.props["MaximumPasses"] = 12
    setup.props["MaxDeltaS"] = 0.02
    sweep = setup.create_frequency_sweep(
        unit="GHz", start_frequency=F_LIMITS_GHZ[0],
        stop_frequency=F_LIMITS_GHZ[1], num_of_freq_points=501,
        name=SWEEP_NAME, sweep_type="Discrete",
    )
    if not sweep:
        raise RuntimeError("HFSS sweep creation failed for %s" % name)
    solved = hfss.analyze_setup(name=SETUP_NAME)
    if not solved:
        raise RuntimeError("HFSS solve failed for %s" % name)
    plot_name = f"{name}_s11_hfss"
    report = hfss.post.create_report(
        expressions=f"dB(S({port_name},{port_name}))",
        setup_sweep_name=f"{SETUP_NAME} : {SWEEP_NAME}",
        plot_name=plot_name,
    )
    if not report:
        raise RuntimeError("HFSS report creation failed for %s" % name)
    csv = hfss.post.export_report_to_csv(out_dir, plot_name)
    if not csv:
        raise RuntimeError("HFSS CSV export failed for %s" % name)
    print("done", name, "->", csv)
    hfss.release_desktop(False, False)
    return csv


def main():
    out_dir = os.path.dirname(os.path.abspath(__file__))
    only = os.environ.get("VARIANT")
    for name in PIECE_NAMES:
        if only and name != only:
            continue
        build_variant(name, out_dir)


if __name__ == "__main__":
    main()
