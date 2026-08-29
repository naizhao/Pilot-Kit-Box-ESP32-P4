#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""HFSS 6 层标准 IFA：短路腿直接入地，扫描 L/edge/stack/D/H。"""

import datetime
import json
import math
import os
import traceback


WORK_DIR = os.path.join(os.path.expanduser("~"), "Documents", "HFSS_IFA_Codex")
STATUS_FILE = os.path.join(WORK_DIR, "ifa_direct_status.json")

# 当前v4坐标中，馈脚6.0mm中心线端点到ZP1.1的路径为5.1032mm。真实铜箔依次是：
# 0.75mm等宽端帽 + 1.50mm完整taper + 2.8532mm的0.15mm微带。
# rev2修复了旧模型把“中心线端点”误当作“铜箔外沿”的0.75mm口径差。
FEED_PATH_TOTAL = 5.1032
LEG_HALF_WIDTH = 0.75
TAPER_LENGTH = 1.5
GEOM_REV = "copperedge_rev2"
# AEDT 会把项目名和设计名同时嵌进结果目录。设计名必须保持短小，
# 否则 Windows 的结果矩阵复制会因路径过长失败。
DESIGN_NAME = "IFA"

DEFAULT_ARM_CENTERLINE = 54.10
DEFAULT_ARM_EDGE_CLEAR = 1.634
DEFAULT_STACK_MODE = "top2"
DEFAULT_LEG_SPACING_D = 5.0
DEFAULT_LEG_CENTERLINE_H = 6.0


def parse_script_argument(script_argument):
    """解析 AEDT ScriptArgument；旧的 L/edge/stack 调用保持 D=5、H=6。"""
    values = (
        DEFAULT_ARM_CENTERLINE,
        DEFAULT_ARM_EDGE_CLEAR,
        DEFAULT_STACK_MODE,
        DEFAULT_LEG_SPACING_D,
        DEFAULT_LEG_CENTERLINE_H,
    )
    if script_argument is None or not str(script_argument).strip():
        return values

    script_args = str(script_argument).split(",")
    arm_centerline = float(script_args[0])
    arm_edge_clear = values[1]
    stack_mode = values[2]
    leg_spacing_d = values[3]
    leg_centerline_h = values[4]
    if len(script_args) > 1 and str(script_args[1]).strip():
        arm_edge_clear = float(script_args[1])
    if len(script_args) > 2 and str(script_args[2]).strip():
        stack_mode = str(script_args[2]).strip().lower()
    if len(script_args) > 3 and str(script_args[3]).strip():
        leg_spacing_d = float(script_args[3])
    if len(script_args) > 4 and str(script_args[4]).strip():
        leg_centerline_h = float(script_args[4])
    if stack_mode not in ("top2", "full6"):
        raise ValueError("STACK_MODE must be top2 or full6, got %r" % stack_mode)
    if leg_spacing_d <= 0:
        raise ValueError("D must be positive, got %r" % leg_spacing_d)
    if leg_centerline_h <= 0:
        raise ValueError("H must be positive, got %r" % leg_centerline_h)
    return (arm_centerline, arm_edge_clear, stack_mode,
            leg_spacing_d, leg_centerline_h)


def _token_number(value, decimals):
    return (("%%.%df" % decimals) % float(value)).replace(".", "p")


def build_case_token(arm_centerline, edge_clear, stack_mode,
                     leg_spacing_d, leg_centerline_h):
    """生成包含所有扫参维度的文件名，防止 D/H 结果互相覆盖。"""
    token = "%s_feed%s" % (
        _token_number(arm_centerline, 2),
        _token_number(FEED_PATH_TOTAL, 2),
    )
    token += "_edge%s" % _token_number(edge_clear, 3)
    token += "_d%s" % _token_number(leg_spacing_d, 3)
    token += "_h%s" % _token_number(leg_centerline_h, 3)
    token += "_%s" % stack_mode
    token += "_%s" % GEOM_REV
    return token


def calculate_geometry(arm_centerline, edge_clear, leg_spacing_d,
                       leg_centerline_h):
    """返回不依赖 AEDT 的关键坐标，供建模和回归测试共用。"""
    board_h = 62.0
    short_x = 70.0
    feed_x = short_x - leg_spacing_d
    arm_y = board_h - edge_clear - LEG_HALF_WIDTH
    leg_center_end_y = arm_y - leg_centerline_h
    leg_copper_end_y = leg_center_end_y - LEG_HALF_WIDTH
    feed_end_y = leg_center_end_y - FEED_PATH_TOTAL
    taper_start_y = leg_copper_end_y
    taper_end_y = taper_start_y - TAPER_LENGTH
    microstrip_length = taper_end_y - feed_end_y
    return {
        "arm_y": arm_y,
        "arm_open_x": short_x - arm_centerline,
        "feed_x": feed_x,
        "short_x": short_x,
        "leg_center_end_y": leg_center_end_y,
        "leg_copper_end_y": leg_copper_end_y,
        "feed_end_y": feed_end_y,
        "taper_start_y": taper_start_y,
        "taper_end_y": taper_end_y,
        "microstrip_length": microstrip_length,
        "slot_bottom": feed_end_y - 0.3,
    }


try:
    _raw_script_argument = ScriptArgument
except NameError:
    _raw_script_argument = None

(ARM_CENTERLINE, ARM_EDGE_CLEAR, STACK_MODE,
 LEG_SPACING_D, LEG_CENTERLINE_H) = parse_script_argument(_raw_script_argument)
CASE_TOKEN = build_case_token(
    ARM_CENTERLINE, ARM_EDGE_CLEAR, STACK_MODE,
    LEG_SPACING_D, LEG_CENTERLINE_H,
)
PROJECT_FILE = os.path.join(WORK_DIR, "ifa_direct_%s.aedt" % CASE_TOKEN)


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
            "XPosition:=", mm(x),
            "YPosition:=", mm(y),
            "ZPosition:=", mm(z),
            "XSize:=", mm(dx),
            "YSize:=", mm(dy),
            "ZSize:=", mm(dz),
        ],
        attributes(name, material, material != "pec", transparency),
    )
    return name


def create_rect(editor, name, x, y, z, width, height, axis="Z",
                material="pec", solve_inside=False):
    editor.CreateRectangle(
        [
            "NAME:RectangleParameters",
            "IsCovered:=", True,
            "XStart:=", mm(x),
            "YStart:=", mm(y),
            "ZStart:=", mm(z),
            "Width:=", mm(width),
            "Height:=", mm(height),
            "WhichAxis:=", axis,
        ],
        attributes(name, material, solve_inside, 0.0),
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
            ["NAME:PLSegment", "SegmentType:=", "Line",
             "StartIndex:=", index, "NoOfPoints:=", 2]
        )
    xsection_args = [
        "NAME:PolylineXSection",
        "XSectionType:=", "None",
        "XSectionOrient:=", "Auto",
        "XSectionWidth:=", "0mm",
        "XSectionTopWidth:=", "0mm",
        "XSectionHeight:=", "0mm",
        "XSectionNumSegments:=", "0",
        "XSectionBendType:=", "Corner",
    ]
    editor.CreatePolyline(
        ["NAME:PolylineParameters", "IsPolylineCovered:=", True,
         "IsPolylineClosed:=", True, point_args, segment_args, xsection_args],
        attributes(name, "pec", False, 0.0),
    )
    return name


def create_region(editor):
    params = ["NAME:RegionParameters"]
    for axis in ["+X", "-X", "+Y", "-Y", "+Z", "-Z"]:
        params.extend([axis + "PaddingType:=", "Absolute Offset"])
        params.extend([axis + "Padding:=", "45mm"])
    editor.CreateRegion(
        params,
        [
            "NAME:Attributes", "Name:=", "Region", "Flags:=", "Wireframe#",
            "Color:=", "(143 175 143)", "Transparency:=", 0.85,
            "PartCoordinateSystem:=", "Global", "UDMId:=", "",
            "MaterialValue:=", '"air"', "SurfaceMaterialValue:=", '""',
            "SolveInside:=", True, "IsMaterialEditable:=", True,
            "UseMaterialAppearance:=", False, "IsLightweight:=", False,
        ],
    )


def build_direct(design, arm_centerline, edge_clear,
                 leg_spacing_d, leg_centerline_h):
    editor = design.SetActiveEditor("3D Modeler")
    editor.SetModelUnits(
        ["NAME:Units Parameter", "Units:=", "mm", "Rescale:=", False]
    )

    board_w = 100.0
    board_h = 62.0
    top_z = 1.5400
    in1_z = top_z - 0.0994
    # JLC06161H-3313 的六铜层位置。保持 F.Cu->In1 的 0.0994mm 与既有模型完全一致，
    # 再按内层铜厚0.0152mm和介质厚度逐层向下放置其余平面，便于做单变量对照。
    in2_z = in1_z - 0.0152 - 0.5500
    in3_z = in2_z - 0.0152 - 0.1088
    in4_z = in3_z - 0.0152 - 0.5500
    bottom_z = in4_z - 0.0152 - 0.0994
    geometry = calculate_geometry(
        arm_centerline, edge_clear, leg_spacing_d, leg_centerline_h
    )
    # edge_clear 是主臂铜箔外沿到板边，不是中心线到板边。
    arm_y = geometry["arm_y"]
    # 与KiCad custom pad逐边对应：H是腿中心线长度，铜箔还向地侧伸出半线宽0.75mm。
    # 深层参考面在馈线通道内可到中心线端点；其它位置及F.Cu地从铜箔外沿开始。
    leg_center_end_y = geometry["leg_center_end_y"]
    leg_copper_end_y = geometry["leg_copper_end_y"]
    feed_x = geometry["feed_x"]
    short_x = geometry["short_x"]
    feed_end_y = geometry["feed_end_y"]
    taper_start_y = geometry["taper_start_y"]
    taper_end_y = geometry["taper_end_y"]
    microstrip_length = geometry["microstrip_length"]
    if abs(microstrip_length - 2.8532) > 1e-6:
        raise ValueError("rev2 microstrip length mismatch: %r" % microstrip_length)

    create_box(editor, "Substrate", 0, 0, 0, board_w, board_h, top_z,
               "FR4_epoxy", 0.75)

    conductors = []
    fine_names = []

    # F.Cu地平面：主体从腿铜箔外沿开始，在馈线处开槽并在端口下方闭合。
    slot_half = 1.2
    slot_bottom = geometry["slot_bottom"]
    top_ground_points = [
        (0, 0), (board_w, 0), (board_w, leg_copper_end_y),
        (feed_x + slot_half, leg_copper_end_y),
        (feed_x + slot_half, slot_bottom),
        (feed_x - slot_half, slot_bottom),
        (feed_x - slot_half, leg_copper_end_y),
        (0, leg_copper_end_y),
    ]
    conductors.append(create_polygon_xy(editor, "GroundTop", top_ground_points, top_z))

    # KiCad的全层keepout在馈电脚下方有2.4mm宽凹口：In1/深层参考面在该凹口内
    # 可前伸到6.0mm中心线端点，其它位置只到腿铜箔外沿。用同一多边形逐层复现。
    deep_ground_points = [
        (0, 0), (board_w, 0), (board_w, leg_copper_end_y),
        (feed_x + slot_half, leg_copper_end_y),
        (feed_x + slot_half, leg_center_end_y),
        (feed_x - slot_half, leg_center_end_y),
        (feed_x - slot_half, leg_copper_end_y),
        (0, leg_copper_end_y),
    ]
    conductors.append(create_polygon_xy(editor, "GroundIn1", deep_ground_points, in1_z))
    if STACK_MODE == "full6":
        # 当前v4 zone事实：In2没有连续平面；In3=3V3_DIG平面；In4=GND；
        # B.Cu以GND为主并含电源岛。1.09GHz下电源面会经去耦呈RF地，本轮先按完整PEC
        # 平面处理；所有深层铜都复用上面的实际凹口边界。
        conductors.append(create_polygon_xy(editor, "PowerIn3", deep_ground_points, in3_z))
        conductors.append(create_polygon_xy(editor, "GroundIn4", deep_ground_points, in4_z))
        conductors.append(create_polygon_xy(editor, "GroundBottom", deep_ground_points, bottom_z))

    arm_open_x = geometry["arm_open_x"]
    arm = create_rect(editor, "Arm", arm_open_x, arm_y - 0.75, top_z,
                      arm_centerline, 1.5)
    feed_leg = create_rect(editor, "FeedLeg", feed_x - 0.75, leg_copper_end_y,
                           top_z, 1.5, leg_centerline_h + LEG_HALF_WIDTH)
    short_leg = create_rect(editor, "ShortLeg", short_x - 0.75, leg_copper_end_y,
                            top_z, 1.5, leg_centerline_h + LEG_HALF_WIDTH)
    conductors.extend([arm, feed_leg, short_leg])
    fine_names.extend([arm, feed_leg, short_leg])

    # 从馈电脚真实铜箔外沿开始做完整1.5mm的1.5→0.15mm渐变；不与端帽重叠。
    taper = create_polygon_xy(
        editor,
        "FeedTaper",
        [
            (feed_x - 0.75, taper_start_y),
            (feed_x + 0.75, taper_start_y),
            (feed_x + 0.075, taper_end_y),
            (feed_x - 0.075, taper_end_y),
        ],
        top_z,
    )
    feedline = create_rect(editor, "FeedLine", feed_x - 0.075, feed_end_y,
                           top_z, 0.15, taper_end_y - feed_end_y)
    conductors.extend([taper, feedline])
    fine_names.extend([taper, feedline])

    # 一个靠近短路腿的等效接地过孔把F.Cu与In1连接；额外引线长度为零。
    via = create_box(editor, "ShortGroundVia", short_x - 0.25,
                     leg_copper_end_y - 0.50, in1_z, 0.50, 0.50,
                     top_z - in1_z, "pec", 0.0)
    conductors.append(via)
    fine_names.append(via)

    # 端口平面垂直于微带方向，积分线从L1信号线指向In1参考地。
    # HFSS 在 WhichAxis=Y 时 Width 是 +Z 尺寸、Height 是 +X 尺寸；
    # 从 In1 向上画到 L1。此规则由 GetObjectBoundingBox 实测确认。
    create_rect(editor, "PortSheet", feed_x - 0.30, feed_end_y, in1_z,
                top_z - in1_z, 0.60, "Y", "vacuum", True)
    port_bbox = [str(value) for value in editor.GetObjectBoundingBox("PortSheet")]

    create_region(editor)

    boundary = design.GetModule("BoundarySetup")
    boundary.AssignPerfectE(
        ["NAME:PEC", "Objects:=", conductors, "InfGroundPlane:=", False]
    )
    try:
        boundary.AssignLumpedPort(
            [
            "NAME:Port1", "Objects:=", ["PortSheet"],
            "DoDeembed:=", False, "RenormalizeAllTerminals:=", True,
            [
                "NAME:Modes",
                [
                    "NAME:Mode1", "ModeNum:=", 1, "UseIntLine:=", True,
                    [
                        "NAME:IntLine",
                        "Start:=", [mm(feed_x), mm(feed_end_y), mm(top_z)],
                        "End:=", [mm(feed_x), mm(feed_end_y), mm(in1_z)],
                    ],
                    "AlignmentGroup:=", 0, "CharImp:=", "Zpi",
                    "RenormImp:=", "50ohm",
                ],
            ],
            "ShowReporterFilter:=", False, "ReporterFilter:=", [True],
            "Impedance:=", "50ohm",
            ]
        )
    except Exception as port_error:
        raise Exception("%r; PortSheet bbox=%r" % (port_error, port_bbox))
    boundary.AssignRadiation(
        ["NAME:Radiation1", "Objects:=", ["Region"],
         "IsFssReference:=", False, "IsForPML:=", False]
    )
    return fine_names


def setup_solve_export(design, fine_names):
    design.GetModule("MeshSetup").AssignLengthOp(
        [
            "NAME:IFA_Fine", "RefineInside:=", False, "Enabled:=", True,
            "Objects:=", fine_names, "RestrictElem:=", False,
            "RestrictLength:=", True, "MaxLength:=", "0.75mm",
        ]
    )
    setup = design.GetModule("AnalysisSetup")
    setup.InsertSetup(
        "HfssDriven",
        [
            "NAME:Setup1", "SolveType:=", "Single", "Frequency:=", "1.10GHz",
            "MaxDeltaS:=", 0.02, "PortsOnly:=", False, "UseMatrixConv:=", False,
            "MaximumPasses:=", 6, "MinimumPasses:=", 1,
            "MinimumConvergedPasses:=", 1, "PercentRefinement:=", 25,
            "IsEnabled:=", True, ["NAME:MeshLink", "ImportMesh:=", False],
            "BasisOrder:=", 1, "DoLambdaRefine:=", True,
            "DoMaterialLambda:=", True, "SetLambdaTarget:=", False,
            "Target:=", 0.3333, "UseMaxTetIncrease:=", False,
            "PortAccuracy:=", 2, "UseABCOnPort:=", False,
            "SetPortMinMaxTri:=", False, "UseDomains:=", False,
            "UseIterativeSolver:=", False, "SaveRadFieldsOnly:=", False,
            "SaveAnyFields:=", True, "IESolverType:=", "Auto",
            "LambdaTargetForIESolver:=", 0.15,
            "UseDefaultLambdaTgtForIESolver:=", True,
        ],
    )
    setup.InsertFrequencySweep(
        "Setup1",
        [
            "NAME:Sweep1", "Type:=", "Interpolating", "IsEnabled:=", True,
            "RangeType:=", "LinearCount", "RangeStart:=", "0.85GHz",
            "RangeEnd:=", "1.30GHz", "RangeCount:=", 91,
            "SaveFields:=", False, "SaveRadFields:=", False,
            "InterpTolerance:=", 0.01, "InterpMaxSolns:=", 100,
            "InterpMinSolns:=", 0, "InterpMinSubranges:=", 1,
            "InterpUseS:=", True, "InterpUsePortImped:=", False,
            "InterpUsePropConst:=", True, "UseDerivativeConvergence:=", False,
            "EnforcePassivity:=", True, "UseFullBasis:=", True,
            "PassivityErrorTolerance:=", 0.0001, "EnforceCausality:=", False,
            "SMatrixOnlySolveMode:=", "Auto", "SMatrixOnlySolveAbove:=", "1MHz",
        ],
    )
    design.AnalyzeAll()
    report = design.GetModule("ReportSetup")
    report.CreateReport(
        "S11", "Modal Solution Data", "Rectangular Plot", "Setup1 : Sweep1",
        ["Domain:=", "Sweep"], ["Freq:=", ["All"]],
        [
            "X Component:=", "Freq", "Y Component:=",
            ["dB(S(Port1,Port1))", "re(S(Port1,Port1))", "im(S(Port1,Port1))"],
        ],
    )
    csv_file = os.path.join(WORK_DIR, "ifa_direct_%s_s11.csv" % CASE_TOKEN)
    report.ExportToFile("S11", csv_file, False)
    return csv_file


def main():
    write_status(state="starting", arm_centerline_mm=ARM_CENTERLINE,
                 edge_clear_mm=ARM_EDGE_CLEAR, stack_mode=STACK_MODE,
                 leg_spacing_d_mm=LEG_SPACING_D,
                 leg_centerline_h_mm=LEG_CENTERLINE_H,
                 geometry_revision=GEOM_REV,
                 project=PROJECT_FILE)
    try:
        import ScriptEnv

        ScriptEnv.Initialize("Ansoft.ElectronicsDesktop")
        project = oDesktop.NewProject()  # noqa: F821 - 由AEDT注入。
        project.InsertDesign("HFSS", DESIGN_NAME, "DrivenModal", "")
        design = project.SetActiveDesign(DESIGN_NAME)
        fine_names = build_direct(
            design, ARM_CENTERLINE, ARM_EDGE_CLEAR,
            LEG_SPACING_D, LEG_CENTERLINE_H,
        )
        project.SaveAs(PROJECT_FILE, True)
        write_status(state="solving", arm_centerline_mm=ARM_CENTERLINE,
                     edge_clear_mm=ARM_EDGE_CLEAR, stack_mode=STACK_MODE,
                     leg_spacing_d_mm=LEG_SPACING_D,
                     leg_centerline_h_mm=LEG_CENTERLINE_H,
                     geometry_revision=GEOM_REV,
                     project=PROJECT_FILE)
        csv_file = setup_solve_export(design, fine_names)
        project.Save()
        write_status(state="solved", arm_centerline_mm=ARM_CENTERLINE,
                     edge_clear_mm=ARM_EDGE_CLEAR, stack_mode=STACK_MODE,
                     leg_spacing_d_mm=LEG_SPACING_D,
                     leg_centerline_h_mm=LEG_CENTERLINE_H,
                     geometry_revision=GEOM_REV,
                     project=PROJECT_FILE, s11_csv=csv_file)
        return 0
    except Exception as exc:
        write_status(state="error", arm_centerline_mm=ARM_CENTERLINE,
                     edge_clear_mm=ARM_EDGE_CLEAR, stack_mode=STACK_MODE,
                     leg_spacing_d_mm=LEG_SPACING_D,
                     leg_centerline_h_mm=LEG_CENTERLINE_H,
                     geometry_revision=GEOM_REV,
                     project=PROJECT_FILE, error=repr(exc),
                     traceback=traceback.format_exc())
        return 1


if __name__ == "__main__" or _raw_script_argument is not None:
    main()
