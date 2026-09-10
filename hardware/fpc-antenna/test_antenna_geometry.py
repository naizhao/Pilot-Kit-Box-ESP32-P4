#!/usr/bin/env python3
"""Pure-Python contracts for both electromagnetic-model entries."""

import contextlib
import math
import os
import sys
import types
import unittest

import hfss_adsb_fpc
import sim_openems


# These coordinates are hand-expanded from HANDOFF_PROMPT.md section 2.  They
# deliberately do not import antenna_geometry.py or gen_panel.py, so a shared
# implementation cannot make an incorrect geometry agree with its own test.
EXPECTED = {
    "S0": {
        "trace_width": 2.0,
        "arm_length": 70.5,
        "left": (
            ((47.0, 14.0), (47.0, 9.0)),
            ((47.0, 9.0), (17.0, 9.0)),
            ((17.0, 9.0), (17.0, 14.0)),
            ((17.0, 14.0), (30.0, 14.0)),
            ((30.0, 14.0), (30.0, 19.0)),
            ((30.0, 19.0), (22.0, 19.0)),
            ((22.0, 19.0), (17.5, 19.0)),
        ),
        "right": (
            ((53.0, 14.0), (53.0, 9.0)),
            ((53.0, 9.0), (83.0, 9.0)),
            ((83.0, 9.0), (83.0, 14.0)),
            ((83.0, 14.0), (70.0, 14.0)),
            ((70.0, 14.0), (70.0, 19.0)),
            ((70.0, 19.0), (78.0, 19.0)),
            ((78.0, 19.0), (82.5, 19.0)),
        ),
        "sig_tongue": ((42.0, 6.5), (50.4, 11.5)),
        "gnd_tongue": ((51.6, 6.5), (58.0, 11.5)),
        "feed_gap": ((50.4, 6.5), (51.6, 11.5)),
    },
    "S-4": {
        "trace_width": 2.0,
        "arm_length": 65.5,
        "left": (
            ((47.0, 37.0), (47.0, 32.0)),
            ((47.0, 32.0), (19.0, 32.0)),
            ((19.0, 32.0), (19.0, 37.0)),
            ((19.0, 37.0), (31.5, 37.0)),
            ((31.5, 37.0), (31.5, 42.0)),
            ((31.5, 42.0), (21.5, 42.0)),
        ),
        "right": (
            ((53.0, 37.0), (53.0, 32.0)),
            ((53.0, 32.0), (81.0, 32.0)),
            ((81.0, 32.0), (81.0, 37.0)),
            ((81.0, 37.0), (68.5, 37.0)),
            ((68.5, 37.0), (68.5, 42.0)),
            ((68.5, 42.0), (78.5, 42.0)),
        ),
        "sig_tongue": ((42.0, 29.5), (50.4, 34.5)),
        "gnd_tongue": ((51.6, 29.5), (58.0, 34.5)),
        "feed_gap": ((50.4, 29.5), (51.6, 34.5)),
    },
    "D0": {
        "trace_width": 2.5,
        "arm_length": 75.5,
        "left": (
            ((47.0, 60.0), (47.0, 55.0)),
            ((47.0, 55.0), (17.0, 55.0)),
            ((17.0, 55.0), (17.0, 60.0)),
            ((17.0, 60.0), (32.0, 60.0)),
            ((32.0, 60.0), (32.0, 65.0)),
            ((32.0, 65.0), (21.0, 65.0)),
            ((21.0, 65.0), (16.5, 65.0)),
        ),
        "right": (
            ((53.0, 60.0), (53.0, 55.0)),
            ((53.0, 55.0), (83.0, 55.0)),
            ((83.0, 55.0), (83.0, 60.0)),
            ((83.0, 60.0), (68.0, 60.0)),
            ((68.0, 60.0), (68.0, 65.0)),
            ((68.0, 65.0), (79.0, 65.0)),
            ((79.0, 65.0), (83.5, 65.0)),
        ),
        "sig_tongue": ((42.0, 52.5), (50.4, 57.5)),
        "gnd_tongue": ((51.6, 52.5), (58.0, 57.5)),
        "feed_gap": ((50.4, 52.5), (51.6, 57.5)),
    },
    "D-5": {
        "trace_width": 2.5,
        "arm_length": 70.0,
        "left": (
            ((47.0, 83.0), (47.0, 78.0)),
            ((47.0, 78.0), (18.0, 78.0)),
            ((18.0, 78.0), (18.0, 83.0)),
            ((18.0, 83.0), (31.0, 83.0)),
            ((31.0, 83.0), (31.0, 88.0)),
            ((31.0, 88.0), (18.0, 88.0)),
        ),
        "right": (
            ((53.0, 83.0), (53.0, 78.0)),
            ((53.0, 78.0), (82.0, 78.0)),
            ((82.0, 78.0), (82.0, 83.0)),
            ((82.0, 83.0), (69.0, 83.0)),
            ((69.0, 83.0), (69.0, 88.0)),
            ((69.0, 88.0), (82.0, 88.0)),
        ),
        "sig_tongue": ((42.0, 75.5), (50.4, 80.5)),
        "gnd_tongue": ((51.6, 75.5), (58.0, 80.5)),
        "feed_gap": ((50.4, 75.5), (51.6, 80.5)),
    },
}


def segment_length(segment):
    (x1, y1), (x2, y2) = segment
    return math.hypot(x2 - x1, y2 - y1)


class _FakeObject:
    def __init__(self, name):
        self.name = name
        self.material_name = None


class _FakeMaterial:
    def __init__(self):
        self.permittivity = None
        self.dielectric_loss_tangent = None


class _FakeMaterials:
    def add_material(self, name):
        return _FakeMaterial()


class _FakeModeler:
    def __init__(self):
        self.model_units = None
        self.objects = {}

    def _object(self, name):
        item = _FakeObject(name)
        self.objects[name] = item
        return item

    def create_box(self, origin, sizes, name, material=None):
        return self._object(name)

    def create_rectangle(self, orientation, origin, sizes, name):
        return self._object(name)

    def unite(self, assignment):
        return assignment[0]

    def __getitem__(self, name):
        return self.objects[name]


class _FakeSweep:
    pass


class _FakeReport:
    pass


class _FakeSetup:
    def __init__(self, runtime):
        self.runtime = runtime
        self.props = {}
        self.sweep_calls = []

    # Keep this signature aligned with the documented SetupHFSS method. In
    # particular, it intentionally has neither ``sweep_name`` nor
    # ``step_size``, so the real entry point cannot hide unsupported keywords.
    def create_frequency_sweep(
            self, unit=None, start_frequency=1.0, stop_frequency=10.0,
            num_of_freq_points=None, name=None, save_fields=True,
            save_rad_fields=False, sweep_type="Discrete",
            interpolation_tol=0.5, interpolation_max_solutions=250):
        self.sweep_calls.append({
            "unit": unit,
            "start_frequency": start_frequency,
            "stop_frequency": stop_frequency,
            "num_of_freq_points": num_of_freq_points,
            "name": name,
            "sweep_type": sweep_type,
        })
        if self.runtime.failure_stage == "sweep":
            return self.runtime.failure_value
        return _FakeSweep()


class _FakePostProcessor3D:
    def __init__(self, runtime):
        self.runtime = runtime
        self.report_calls = []
        self.export_calls = []

    # Keep this signature aligned with PostProcessor3D.create_report.
    def create_report(
            self, expressions=None, setup_sweep_name=None, domain="Sweep",
            variations=None, primary_sweep_variable=None,
            secondary_sweep_variable=None, report_category=None,
            plot_type="Rectangular Plot", context=None, subdesign_id=None,
            polyline_points=1001, plot_name=None, matplotlib=False, show=True,
            hide_legend=False, snapshot_path=None, width=800, height=450):
        self.report_calls.append({
            "expressions": expressions,
            "setup_sweep_name": setup_sweep_name,
            "plot_name": plot_name,
        })
        if self.runtime.failure_stage == "report":
            return self.runtime.failure_value
        return _FakeReport()

    # The third positional argument is ``uniform``, not an output filename.
    # PyAEDT selects the output filename from ``plot_name`` and returns it.
    def export_report_to_csv(
            self, project_dir, plot_name, uniform=False, start=None, end=None,
            step=None, use_trace_number_format=False):
        self.export_calls.append({
            "project_dir": project_dir,
            "plot_name": plot_name,
            "uniform": uniform,
        })
        if self.runtime.failure_stage == "export":
            return self.runtime.failure_value
        return os.path.join(project_dir, plot_name + ".csv")


class _FakeHfss:
    def __init__(self, runtime, api, project, solution_type, design,
                 new_desktop):
        self.runtime = runtime
        self.api = api
        self.project = project
        self.solution_type = solution_type
        self.design = design
        self.new_desktop = new_desktop
        if api == "current":
            self.axis_directions = types.SimpleNamespace(XNeg="current-XNeg")
        else:
            self.AxisDir = types.SimpleNamespace(XNeg="legacy-XNeg")
        self.modeler = _FakeModeler()
        self.materials = _FakeMaterials()
        self.post = _FakePostProcessor3D(runtime)
        self.setup = None
        self.analyze_calls = []
        self.port_calls = []
        self.port_results = []
        self.release_calls = []
        runtime.instances.append(self)

    def assign_material(self, assignment, material):
        return True

    def assign_perfecte_to_sheets(self, assignment, name=None):
        return True

    # Keep this signature aligned with the current Hfss.lumped_port API.  The
    # first positional argument works on both current and legacy PyAEDT, while
    # the removed ``signal`` keyword must raise TypeError in this boundary fake.
    def lumped_port(
            self, assignment, reference=None, create_port_sheet=False,
            port_on_plane=True, integration_line=0, impedance=50, name=None,
            renormalize=True, deembed=False, terminals_rename=True,
            auto_identify=False):
        self.port_calls.append({
            "assignment": assignment,
            "integration_line": integration_line,
            "impedance": impedance,
            "name": name,
        })
        if self.runtime.failure_stage == "port":
            result = self.runtime.failure_value
        else:
            result = _FakeObject(self.runtime.port_return_name or name)
        self.port_results.append(result.name if result else result)
        return result

    def assign_radiation_boundary_to_objects(self, assignment):
        return True

    def create_setup(self, name):
        self.setup = _FakeSetup(self.runtime)
        return self.setup

    def analyze_setup(
            self, name=None, cores=None, tasks=None, gpus=None, acf_file=None,
            use_auto_settings=True, num_variations_to_distribute=None,
            allowed_distribution_types=None, revert_to_initial_mesh=False,
            blocking=True):
        self.analyze_calls.append({
            "method": "analyze_setup",
            "name": name,
        })
        if self.runtime.failure_stage == "solve":
            return self.runtime.failure_value
        return True

    def release_desktop(self, close_projects=True, close_desktop=True):
        self.release_calls.append((close_projects, close_desktop))
        return True


class _FakePyaedtRuntime:
    def __init__(self, failure_stage=None, failure_value=None,
                 port_return_name=None):
        self.failure_stage = failure_stage
        self.failure_value = failure_value
        self.port_return_name = port_return_name
        self.instances = []
        self.constructor_calls = []


@contextlib.contextmanager
def installed_fake_aedt(runtime, api="current"):
    """Install one current or legacy in-memory PyAEDT boundary double."""
    missing = object()
    module_names = ("ansys", "ansys.aedt", "ansys.aedt.core", "pyaedt")
    previous = {name: sys.modules.get(name, missing) for name in module_names}

    if api == "current":
        ansys_module = types.ModuleType("ansys")
        ansys_module.__path__ = []
        aedt_module = types.ModuleType("ansys.aedt")
        aedt_module.__path__ = []
        core_module = types.ModuleType("ansys.aedt.core")

        class Hfss(_FakeHfss):
            def __init__(
                    self, project=None, design=None, solution_type=None,
                    setup=None, version=None, non_graphical=False,
                    new_desktop=False, close_on_exit=False,
                    student_version=False, machine="", port=0,
                    aedt_process_id=None, remove_lock=False):
                runtime.constructor_calls.append({
                    "api": "current",
                    "project": project,
                    "design": design,
                    "solution_type": solution_type,
                    "new_desktop": new_desktop,
                })
                super().__init__(
                    runtime, "current", project, solution_type, design,
                    new_desktop,
                )

        core_module.Hfss = Hfss
        ansys_module.aedt = aedt_module
        aedt_module.core = core_module
        sys.modules["ansys"] = ansys_module
        sys.modules["ansys.aedt"] = aedt_module
        sys.modules["ansys.aedt.core"] = core_module
        # A current-only installation must expose accidental legacy imports.
        sys.modules["pyaedt"] = None
    elif api == "legacy":
        module = types.ModuleType("pyaedt")

        class Hfss(_FakeHfss):
            def __init__(self, projectname=None, solution_type=None,
                         designname=None, new_desktop_session=False):
                runtime.constructor_calls.append({
                    "api": "legacy",
                    "projectname": projectname,
                    "designname": designname,
                    "solution_type": solution_type,
                    "new_desktop_session": new_desktop_session,
                })
                super().__init__(
                    runtime, "legacy", projectname, solution_type, designname,
                    new_desktop_session,
                )

        module.Hfss = Hfss
        sys.modules["ansys.aedt.core"] = None
        sys.modules["pyaedt"] = module
    else:
        raise ValueError("Unknown fake PyAEDT API: %s" % api)

    try:
        yield
    finally:
        for name, old_module in previous.items():
            if old_module is missing:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = old_module


class SimulationGeometryContractTests(unittest.TestCase):
    ENTRY_POINTS = (
        ("openEMS", sim_openems.simulation_geometry),
        ("HFSS", hfss_adsb_fpc.simulation_geometry),
    )

    def test_both_simulators_expose_the_frozen_left_and_right_arms(self):
        for engine, entry_point in self.ENTRY_POINTS:
            for name, expected in EXPECTED.items():
                with self.subTest(engine=engine, variant=name, side="left"):
                    geometry = entry_point(name)
                    self.assertEqual(geometry.left_arm, expected["left"])
                with self.subTest(engine=engine, variant=name, side="right"):
                    self.assertEqual(geometry.right_arm, expected["right"])

    def test_each_simulated_arm_has_the_frozen_segment_count_and_length(self):
        for engine, entry_point in self.ENTRY_POINTS:
            for name, expected in EXPECTED.items():
                geometry = entry_point(name)
                expected_count = 7 if name in ("S0", "D0") else 6
                for side, segments in (("left", geometry.left_arm),
                                       ("right", geometry.right_arm)):
                    with self.subTest(engine=engine, variant=name, side=side):
                        self.assertEqual(len(segments), expected_count)
                        self.assertAlmostEqual(
                            sum(segment_length(segment) for segment in segments),
                            expected["arm_length"], places=9,
                        )

    def test_each_simulated_right_arm_is_the_x50_mirror_of_its_left_arm(self):
        for engine, entry_point in self.ENTRY_POINTS:
            for name in EXPECTED:
                geometry = entry_point(name)
                mirrored = tuple(
                    tuple((100.0 - x, y) for x, y in segment)
                    for segment in geometry.left_arm
                )
                with self.subTest(engine=engine, variant=name):
                    self.assertEqual(geometry.right_arm, mirrored)

    def test_both_simulators_use_the_frozen_top_edge_tongues_and_feed_gap(self):
        for engine, entry_point in self.ENTRY_POINTS:
            for name, expected in EXPECTED.items():
                geometry = entry_point(name)
                with self.subTest(engine=engine, variant=name):
                    self.assertEqual(geometry.trace_width, expected["trace_width"])
                    self.assertEqual(geometry.sig_tongue, expected["sig_tongue"])
                    self.assertEqual(geometry.gnd_tongue, expected["gnd_tongue"])
                    self.assertEqual(geometry.feed_gap, expected["feed_gap"])
                    self.assertAlmostEqual(
                        geometry.feed_gap[1][0] - geometry.feed_gap[0][0],
                        1.2, places=9,
                    )


class HfssBuildContractTests(unittest.TestCase):
    OUT_DIR = "/virtual/hfss-output"

    def build_with_runtime(self, name, runtime, api="current"):
        with installed_fake_aedt(runtime, api):
            try:
                return hfss_adsb_fpc.build_variant(name, self.OUT_DIR)
            except Exception as error:
                self.fail(
                    f"build_variant({name!r}) violated the PyAEDT contract: "
                    f"{type(error).__name__}: {error}"
                )

    def test_all_variants_use_exact_solve_report_and_export_contracts(self):
        returned_paths = []
        plot_names = []
        expected_port_names = {
            "S0": "P_S0",
            "S-4": "P_Sm4",
            "D0": "P_D0",
            "D-5": "P_Dm5",
        }
        for name in EXPECTED:
            runtime = _FakePyaedtRuntime()
            with self.subTest(variant=name):
                returned_paths.append(self.build_with_runtime(name, runtime))
                self.assertEqual(len(runtime.instances), 1)
                app = runtime.instances[0]
                self.assertEqual(runtime.constructor_calls, [{
                    "api": "current",
                    "project": os.path.join(
                        self.OUT_DIR,
                        "adsb_fpc_" + name.replace("-", "m") + ".aedt",
                    ),
                    "design": name,
                    "solution_type": "Modal",
                    "new_desktop": False,
                }])
                self.assertEqual(app.setup.sweep_calls, [{
                    "unit": "GHz",
                    "start_frequency": 0.8,
                    "stop_frequency": 1.3,
                    "num_of_freq_points": 501,
                    "name": "sw0800_1300",
                    "sweep_type": "Discrete",
                }])
                self.assertEqual(app.analyze_calls, [{
                    "method": "analyze_setup",
                    "name": "Sweep1090",
                }])
                port_name = expected_port_names[name]
                self.assertEqual(app.port_calls, [{
                    "assignment": "port_" + name,
                    "integration_line": "current-XNeg",
                    "impedance": 50,
                    "name": port_name,
                }])
                self.assertEqual(app.port_results, [port_name])
                expected_plot_name = name + "_s11_hfss"
                self.assertEqual(app.post.report_calls, [{
                    "expressions": f"dB(S({port_name},{port_name}))",
                    "setup_sweep_name": "Sweep1090 : sw0800_1300",
                    "plot_name": expected_plot_name,
                }])
                plot_name = app.post.report_calls[0]["plot_name"]
                plot_names.append(plot_name)
                self.assertEqual(app.post.export_calls, [{
                    "project_dir": self.OUT_DIR,
                    "plot_name": plot_name,
                    "uniform": False,
                }])
                self.assertEqual(
                    returned_paths[-1],
                    os.path.join(self.OUT_DIR, plot_name + ".csv"),
                )

        self.assertEqual(len(set(plot_names)), len(EXPECTED))
        self.assertEqual(len(set(returned_paths)), len(EXPECTED))
        self.assertEqual(
            set(returned_paths),
            {os.path.join(self.OUT_DIR, name + "_s11_hfss.csv")
             for name in EXPECTED},
        )

    def test_legacy_import_constructor_and_axis_direction_remain_supported(self):
        runtime = _FakePyaedtRuntime()
        returned_path = self.build_with_runtime("S-4", runtime, api="legacy")
        self.assertEqual(runtime.constructor_calls, [{
            "api": "legacy",
            "projectname": os.path.join(
                self.OUT_DIR, "adsb_fpc_Sm4.aedt",
            ),
            "designname": "S-4",
            "solution_type": "Modal",
            "new_desktop_session": False,
        }])
        app = runtime.instances[0]
        self.assertEqual(app.port_calls, [{
            "assignment": "port_S-4",
            "integration_line": "legacy-XNeg",
            "impedance": 50,
            "name": "P_Sm4",
        }])
        self.assertEqual(
            app.post.report_calls[0]["expressions"],
            "dB(S(P_Sm4,P_Sm4))",
        )
        self.assertEqual(
            returned_path,
            os.path.join(self.OUT_DIR, "S-4_s11_hfss.csv"),
        )

    def test_reports_target_the_created_setup_and_sweep(self):
        for name in EXPECTED:
            runtime = _FakePyaedtRuntime()
            with self.subTest(variant=name):
                self.build_with_runtime(name, runtime)
                self.assertEqual(
                    runtime.instances[0].post.report_calls[0]["setup_sweep_name"],
                    "Sweep1090 : sw0800_1300",
                )

    def test_report_expression_uses_name_returned_by_lumped_port(self):
        runtime = _FakePyaedtRuntime(port_return_name="AEDT_Resolved_Port")
        self.build_with_runtime("S0", runtime)
        self.assertEqual(runtime.instances[0].port_results,
                         ["AEDT_Resolved_Port"])
        self.assertEqual(
            runtime.instances[0].post.report_calls[0]["expressions"],
            "dB(S(AEDT_Resolved_Port,AEDT_Resolved_Port))",
        )

    def test_port_sweep_solve_report_and_export_fail_fast_on_falsy_results(self):
        for stage in ("port", "sweep", "solve", "report", "export"):
            for failure_value in (False, ""):
                runtime = _FakePyaedtRuntime(stage, failure_value)
                with self.subTest(stage=stage, failure_value=repr(failure_value)):
                    with installed_fake_aedt(runtime):
                        try:
                            hfss_adsb_fpc.build_variant("S0", self.OUT_DIR)
                        except RuntimeError as error:
                            self.assertIn(stage, str(error).lower())
                        except Exception as error:
                            self.fail(
                                f"{stage} failure did not fail via RuntimeError: "
                                f"{type(error).__name__}: {error}"
                            )
                        else:
                            self.fail(f"{stage} failure did not stop build_variant")

                    self.assertEqual(len(runtime.instances), 1)
                    app = runtime.instances[0]
                    if stage == "port":
                        self.assertEqual(len(app.port_calls), 1)
                        self.assertIsNone(app.setup)
                        self.assertEqual(app.analyze_calls, [])
                        self.assertEqual(app.post.report_calls, [])
                        self.assertEqual(app.post.export_calls, [])
                    elif stage == "sweep":
                        self.assertEqual(app.analyze_calls, [])
                        self.assertEqual(app.post.report_calls, [])
                        self.assertEqual(app.post.export_calls, [])
                    elif stage == "solve":
                        self.assertEqual(len(app.analyze_calls), 1)
                        self.assertEqual(app.post.report_calls, [])
                        self.assertEqual(app.post.export_calls, [])
                    elif stage == "report":
                        self.assertEqual(len(app.analyze_calls), 1)
                        self.assertEqual(len(app.post.report_calls), 1)
                        self.assertEqual(app.post.export_calls, [])
                    else:
                        self.assertEqual(len(app.analyze_calls), 1)
                        self.assertEqual(len(app.post.report_calls), 1)
                        self.assertEqual(len(app.post.export_calls), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
