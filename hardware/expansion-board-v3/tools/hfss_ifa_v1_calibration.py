#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""建立 V1 IFA 裸板 HFSS 标定模型。

本脚本由 AEDT ``-RunScriptAndExit`` 原生接口执行，兼容其 IronPython。
第一阶段只建立几何、端口和边界并保存工程；验证模型无误后再启用求解。
"""

import datetime
import json
import math
import os
import traceback


WORK_DIR = os.path.join(os.path.expanduser("~"), "Documents", "HFSS_IFA_Codex")
STATUS_FILE = os.path.join(WORK_DIR, "ifa_v1_calibration_status.json")

# 用户实测切后水平铜包络 42.70 mm；1.5 mm 线宽换算中心线 41.20 mm。
ARM_CENTERLINE = 41.20
try:
    ARM_CENTERLINE = float(ScriptArgument)
except (NameError, TypeError, ValueError):
    pass
CASE_TOKEN = ("%.2f" % ARM_CENTERLINE).replace(".", "p")
PROJECT_FILE = os.path.join(WORK_DIR, "ifa_v1_%s.aedt" % CASE_TOKEN)
BUILD_ONLY = False


def mm(value):
    return "%.6fmm" % float(value)


def write_status(**values):
    if not os.path.isdir(WORK_DIR):
        os.makedirs(WORK_DIR)
    payload = {"updated_at": datetime.datetime.utcnow().isoformat() + "Z"}
    payload.update(values)
    with open(STATUS_FILE, "w") as status_handle:
        status_handle.write(json.dumps(payload, ensure_ascii=True, indent=2))


def attributes(name, material="vacuum", solve_inside=True, transparency=0.0):
    return [
        "NAME:Attributes",
        "Name:=",
        name,
        "Flags:=",
        "",
        "Color:=",
        "(255 128 0)" if material == "pec" else "(143 175 143)",
        "Transparency:=",
        transparency,
        "PartCoordinateSystem:=",
        "Global",
        "UDMId:=",
        "",
        "MaterialValue:=",
        '"%s"' % material,
        "SurfaceMaterialValue:=",
        '""',
        "SolveInside:=",
        solve_inside,
        "ShellElement:=",
        False,
        "ShellElementThickness:=",
        "0mm",
        "IsMaterialEditable:=",
        True,
        "UseMaterialAppearance:=",
        False,
        "IsLightweight:=",
        False,
    ]


def create_box(editor, name, x, y, z, dx, dy, dz, material, transparency=0.0):
    editor.CreateBox(
        [
            "NAME:BoxParameters",
            "XPosition:=",
            mm(x),
            "YPosition:=",
            mm(y),
            "ZPosition:=",
            mm(z),
            "XSize:=",
            mm(dx),
            "YSize:=",
            mm(dy),
            "ZSize:=",
            mm(dz),
        ],
        attributes(name, material, True, transparency),
    )


def create_rect_xy(editor, name, x, y, z, dx, dy):
    editor.CreateRectangle(
        [
            "NAME:RectangleParameters",
            "IsCovered:=",
            True,
            "XStart:=",
            mm(x),
            "YStart:=",
            mm(y),
            "ZStart:=",
            mm(z),
            "Width:=",
            mm(dx),
            "Height:=",
            mm(dy),
            "WhichAxis:=",
            "Z",
        ],
        attributes(name, "pec", False, 0.0),
    )
    return name


def create_polygon_xy(editor, name, points, z):
    closed_points = list(points) + [points[0]]
    point_args = ["NAME:PolylinePoints"]
    for x_pos, y_pos in closed_points:
        point_args.append(
            ["NAME:PLPoint", "X:=", mm(x_pos), "Y:=", mm(y_pos), "Z:=", mm(z)]
        )
    segment_args = ["NAME:PolylineSegments"]
    for index in range(len(points)):
        segment_args.append(
            [
                "NAME:PLSegment",
                "SegmentType:=",
                "Line",
                "StartIndex:=",
                index,
                "NoOfPoints:=",
                2,
            ]
        )
    xsection_args = [
        "NAME:PolylineXSection",
        "XSectionType:=",
        "None",
        "XSectionOrient:=",
        "Auto",
        "XSectionWidth:=",
        "0mm",
        "XSectionTopWidth:=",
        "0mm",
        "XSectionHeight:=",
        "0mm",
        "XSectionNumSegments:=",
        "0",
        "XSectionBendType:=",
        "Corner",
    ]
    editor.CreatePolyline(
        [
            "NAME:PolylineParameters",
            "IsPolylineCovered:=",
            True,
            "IsPolylineClosed:=",
            True,
            point_args,
            segment_args,
            xsection_args,
        ],
        attributes(name, "pec", False, 0.0),
    )
    return name


def create_trace_segment(editor, name, p1, p2, width, z):
    x1, y1 = p1
    x2, y2 = p2
    length = math.hypot(x2 - x1, y2 - y1)
    nx = -(y2 - y1) * width / (2.0 * length)
    ny = (x2 - x1) * width / (2.0 * length)
    return create_polygon_xy(
        editor,
        name,
        [(x1 + nx, y1 + ny), (x2 + nx, y2 + ny),
         (x2 - nx, y2 - ny), (x1 - nx, y1 - ny)],
        z,
    )


def create_region(editor):
    params = ["NAME:RegionParameters"]
    for axis in ["+X", "-X", "+Y", "-Y", "+Z", "-Z"]:
        params.extend([axis + "PaddingType:=", "Absolute Offset"])
        params.extend([axis + "Padding:=", "45mm"])
    attrs = [
        "NAME:Attributes",
        "Name:=",
        "Region",
        "Flags:=",
        "Wireframe#",
        "Color:=",
        "(143 175 143)",
        "Transparency:=",
        0.85,
        "PartCoordinateSystem:=",
        "Global",
        "UDMId:=",
        "",
        "MaterialValue:=",
        '"air"',
        "SurfaceMaterialValue:=",
        '""',
        "SolveInside:=",
        True,
        "IsMaterialEditable:=",
        True,
        "UseMaterialAppearance:=",
        False,
        "IsLightweight:=",
        False,
    ]
    editor.CreateRegion(params, attrs)


def build_v1(design, arm_centerline):
    editor = design.SetActiveEditor("3D Modeler")
    editor.SetModelUnits(
        ["NAME:Units Parameter", "Units:=", "mm", "Rescale:=", False]
    )

    # JLC0216A：上下铜面间介质 1.4300 mm。铜用零厚 PEC sheet，避免薄铜拖垮网格。
    top_z = 1.4300
    create_box(editor, "Substrate", 0, 0, 0, 120, 80, top_z,
               "FR4_epoxy", 0.75)

    conductors = []
    conductors.append(create_rect_xy(editor, "GroundTop", 0, 0, top_z, 120, 60.292))
    conductors.append(create_rect_xy(editor, "GroundBottom", 0, 0, 0, 120, 60.292))

    short_x = 64.805
    feed_x = 59.805
    arm_y = 77.616
    leg_y = 71.616
    arm_open_x = short_x - arm_centerline

    conductors.append(create_rect_xy(editor, "Arm", arm_open_x, arm_y - 0.75,
                                     top_z, arm_centerline, 1.5))
    conductors.append(create_rect_xy(editor, "FeedLeg", feed_x - 0.75, leg_y,
                                     top_z, 1.5, 6.0))
    conductors.append(create_rect_xy(editor, "ShortLeg", short_x - 0.75, leg_y,
                                     top_z, 1.5, 6.0))
    conductors.append(create_rect_xy(editor, "FeedPad", 59.817 - 0.75,
                                     71.628 - 0.75, top_z, 1.5, 1.5))
    conductors.append(create_rect_xy(editor, "ShortPad", 64.805 - 0.75,
                                     71.65125 - 0.75, top_z, 1.5, 1.5))

    # V1 U.FL：一个信号焊盘和三个地焊盘。
    conductors.append(create_rect_xy(editor, "UFLSignal", 61.976 - 0.5,
                                     68.93687 - 0.525, top_z, 1.0, 1.05))
    conductors.append(create_rect_xy(editor, "UFLGroundC", 61.976 - 0.5,
                                     65.93713 - 0.525, top_z, 1.0, 1.05))
    conductors.append(create_rect_xy(editor, "UFLGroundL", 60.50102 - 0.525,
                                     67.437 - 1.1, top_z, 1.05, 2.2))
    conductors.append(create_rect_xy(editor, "UFLGroundR", 63.45098 - 0.525,
                                     67.437 - 1.1, top_z, 1.05, 2.2))

    feed_path = [(61.976, 68.93687), (61.976, 69.469), (60.54490, 70.90010)]
    for index in range(len(feed_path) - 1):
        conductors.append(create_trace_segment(editor, "FeedTrace%d" % index,
                                               feed_path[index], feed_path[index + 1],
                                               0.254, top_z))

    short_path = [(64.805, 71.65125), (64.805, 68.742), (63.754, 67.691)]
    for index in range(len(short_path) - 1):
        conductors.append(create_trace_segment(editor, "ShortTrace%d" % index,
                                               short_path[index], short_path[index + 1],
                                               0.254, top_z))

    ground_segments = [
        ((63.45098, 67.437), (61.976, 65.93713)),
        ((60.50102, 67.437), (61.976, 65.93713)),
        ((61.976, 65.93713), (61.976, 58.420)),
    ]
    for index, endpoints in enumerate(ground_segments):
        conductors.append(create_trace_segment(editor, "GroundTrace%d" % index,
                                               endpoints[0], endpoints[1], 0.508, top_z))

    # 端口跨 U.FL 中央信号焊盘与后方地焊盘之间的真实缝隙。
    editor.CreateRectangle(
        [
            "NAME:RectangleParameters",
            "IsCovered:=",
            True,
            "XStart:=",
            mm(61.726),
            "YStart:=",
            mm(66.46213),
            "ZStart:=",
            mm(top_z),
            "Width:=",
            mm(0.5),
            "Height:=",
            mm(1.94974),
            "WhichAxis:=",
            "Z",
        ],
        attributes("PortSheet", "vacuum", True, 0.0),
    )

    create_region(editor)

    boundary = design.GetModule("BoundarySetup")
    boundary.AssignPerfectE(
        ["NAME:PEC", "Objects:=", conductors, "InfGroundPlane:=", False]
    )
    boundary.AssignLumpedPort(
        [
            "NAME:Port1",
            "Objects:=",
            ["PortSheet"],
            "DoDeembed:=",
            False,
            "RenormalizeAllTerminals:=",
            True,
            [
                "NAME:Modes",
                [
                    "NAME:Mode1",
                    "ModeNum:=",
                    1,
                    "UseIntLine:=",
                    True,
                    [
                        "NAME:IntLine",
                        "Start:=",
                        [mm(61.976), mm(68.41187), mm(top_z)],
                        "End:=",
                        [mm(61.976), mm(66.46213), mm(top_z)],
                    ],
                    "AlignmentGroup:=",
                    0,
                    "CharImp:=",
                    "Zpi",
                    "RenormImp:=",
                    "50ohm",
                ],
            ],
            "ShowReporterFilter:=",
            False,
            "ReporterFilter:=",
            [True],
            "Impedance:=",
            "50ohm",
        ]
    )
    boundary.AssignRadiation(
        [
            "NAME:Radiation1",
            "Objects:=",
            ["Region"],
            "IsFssReference:=",
            False,
            "IsForPML:=",
            False,
        ]
    )
    return conductors


def add_setup_and_solve(design, conductor_names):
    fine_names = [name for name in conductor_names
                  if name not in ("GroundTop", "GroundBottom")]
    mesh = design.GetModule("MeshSetup")
    mesh.AssignLengthOp(
        [
            "NAME:IFA_Fine",
            "RefineInside:=",
            False,
            "Enabled:=",
            True,
            "Objects:=",
            fine_names,
            "RestrictElem:=",
            False,
            "RestrictLength:=",
            True,
            "MaxLength:=",
            "1mm",
        ]
    )
    setup = design.GetModule("AnalysisSetup")
    setup.InsertSetup(
        "HfssDriven",
        [
            "NAME:Setup1",
            "SolveType:=",
            "Single",
            "Frequency:=",
            "1.09GHz",
            "MaxDeltaS:=",
            0.02,
            "PortsOnly:=",
            False,
            "UseMatrixConv:=",
            False,
            "MaximumPasses:=",
            6,
            "MinimumPasses:=",
            1,
            "MinimumConvergedPasses:=",
            1,
            "PercentRefinement:=",
            25,
            "IsEnabled:=",
            True,
            ["NAME:MeshLink", "ImportMesh:=", False],
            "BasisOrder:=",
            1,
            "DoLambdaRefine:=",
            True,
            "DoMaterialLambda:=",
            True,
            "SetLambdaTarget:=",
            False,
            "Target:=",
            0.3333,
            "UseMaxTetIncrease:=",
            False,
            "PortAccuracy:=",
            2,
            "UseABCOnPort:=",
            False,
            "SetPortMinMaxTri:=",
            False,
            "UseDomains:=",
            False,
            "UseIterativeSolver:=",
            False,
            "SaveRadFieldsOnly:=",
            False,
            "SaveAnyFields:=",
            True,
            "IESolverType:=",
            "Auto",
            "LambdaTargetForIESolver:=",
            0.15,
            "UseDefaultLambdaTgtForIESolver:=",
            True,
        ],
    )
    setup.InsertFrequencySweep(
        "Setup1",
        [
            "NAME:Sweep1",
            "Type:=",
            "Interpolating",
            "IsEnabled:=",
            True,
            "RangeType:=",
            "LinearCount",
            "RangeStart:=",
            "0.70GHz",
            "RangeEnd:=",
            "1.30GHz",
            "RangeCount:=",
            121,
            "SaveFields:=",
            False,
            "SaveRadFields:=",
            False,
            "InterpTolerance:=",
            0.01,
            "InterpMaxSolns:=",
            100,
            "InterpMinSolns:=",
            0,
            "InterpMinSubranges:=",
            1,
            "InterpUseS:=",
            True,
            "InterpUsePortImped:=",
            False,
            "InterpUsePropConst:=",
            True,
            "UseDerivativeConvergence:=",
            False,
            "EnforcePassivity:=",
            True,
            "UseFullBasis:=",
            True,
            "PassivityErrorTolerance:=",
            0.0001,
            "EnforceCausality:=",
            False,
            "SMatrixOnlySolveMode:=",
            "Auto",
            "SMatrixOnlySolveAbove:=",
            "1MHz",
        ],
    )
    design.AnalyzeAll()


def export_s11(design):
    report = design.GetModule("ReportSetup")
    report.CreateReport(
        "S11",
        "Modal Solution Data",
        "Rectangular Plot",
        "Setup1 : Sweep1",
        ["Domain:=", "Sweep"],
        ["Freq:=", ["All"]],
        [
            "X Component:=",
            "Freq",
            "Y Component:=",
            [
                "dB(S(Port1,Port1))",
                "re(S(Port1,Port1))",
                "im(S(Port1,Port1))",
            ],
        ],
    )
    csv_file = os.path.join(WORK_DIR, "ifa_v1_%s_s11.csv" % CASE_TOKEN)
    report.ExportToFile("S11", csv_file, False)
    return csv_file


def main():
    write_status(state="starting", build_only=BUILD_ONLY,
                 arm_centerline_mm=ARM_CENTERLINE, project=PROJECT_FILE)
    try:
        import ScriptEnv

        ScriptEnv.Initialize("Ansoft.ElectronicsDesktop")
        project = oDesktop.NewProject()  # noqa: F821 - 由 AEDT 注入。
        design_name = "V1_Arm_%s" % CASE_TOKEN
        project.InsertDesign("HFSS", design_name, "DrivenModal", "")
        design = project.SetActiveDesign(design_name)
        conductor_names = build_v1(design, ARM_CENTERLINE)
        project.SaveAs(PROJECT_FILE, True)
        write_status(state="built", build_only=BUILD_ONLY,
                     arm_centerline_mm=ARM_CENTERLINE, project=PROJECT_FILE,
                     conductor_count=len(conductor_names))
        if not BUILD_ONLY:
            write_status(state="solving", build_only=BUILD_ONLY,
                         arm_centerline_mm=ARM_CENTERLINE, project=PROJECT_FILE)
            add_setup_and_solve(design, conductor_names)
            project.Save()
            csv_file = export_s11(design)
            write_status(state="solved", build_only=BUILD_ONLY,
                         arm_centerline_mm=ARM_CENTERLINE, project=PROJECT_FILE,
                         s11_csv=csv_file)
        return 0
    except Exception as exc:
        write_status(state="error", build_only=BUILD_ONLY,
                     arm_centerline_mm=ARM_CENTERLINE, project=PROJECT_FILE,
                     error=repr(exc), traceback=traceback.format_exc())
        return 1


main()
