"""V4 分件实几何与参数行为；原模型只作为不变部分的比较基准。"""
import json
import math
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_case import SCAD as ORIGINAL, OPENSCAD, scad_run, triangles, contains
from export_a1mini import validate_mesh

HERE = Path(__file__).resolve().parent
SCAD = HERE / 'pilot-kit-box-43-base-v4.scad'

def evaluate(expression, defines=(), source=SCAD):
    with tempfile.TemporaryDirectory(prefix='case-v4-value-') as tmp:
        path = Path(tmp)
        src = path / 'value.scad'
        src.write_text(f'include <{source}>\necho("__V4__", {expression});\n')
        result = scad_run(src, path/'value.csg', ['part=""', *defines])
        match = re.search(r'^ECHO: "__V4__", (.+)$', result.stderr, re.M)
        return json.loads(match[1].replace('undef', 'null'))

def signed_volume(mesh):
    return sum(sum(a[i]*(b[(i+1)%3]*c[(i+2)%3]-b[(i+2)%3]*c[(i+1)%3])
                   for i in range(3))/6 for a,b,c in mesh)


def render_mesh(body, defines=(), allow_empty=False):
    with tempfile.TemporaryDirectory(prefix='case-v4-world-') as tmp:
        src, out = Path(tmp)/'world.scad', Path(tmp)/'world.stl'
        src.write_text(f'include <{SCAD}>\n{body}\n')
        args = [OPENSCAD, '--backend=Manifold', '-o', str(out)]
        for define in ['part=""', 'labels_enabled=false', *defines]:
            args += ['-D', define]
        result = subprocess.run(args+[str(src)], capture_output=True, text=True, timeout=180)
        if re.search(r'^(WARNING|ERROR):', result.stderr, re.M):
            raise AssertionError(result.stderr)
        if allow_empty and 'Current top level object is empty' in result.stderr:
            return []
        if result.returncode:
            raise AssertionError(result.stderr)
        return triangles(out)


def component_bounds(mesh):
    """排版中互相分离的每个实体分别求包围盒，不能用全局最低点掩盖悬空件。"""
    parents=list(range(len(mesh)))
    def root(i):
        while parents[i]!=i:
            parents[i]=parents[parents[i]]
            i=parents[i]
        return i
    seen={}
    for i,tri in enumerate(mesh):
        for point in tri:
            key=tuple(round(v,6) for v in point)
            if key in seen:
                parents[root(i)]=root(seen[key])
            else:
                seen[key]=i
    groups={}
    for i,tri in enumerate(mesh):
        groups.setdefault(root(i),[]).extend(tri)
    return [(tuple(min(p[a] for p in points) for a in range(3)),
             tuple(max(p[a] for p in points) for a in range(3))) for points in groups.values()]


class V4GeometryTests(unittest.TestCase):
    def test_top_corners_are_full_quarter_circles_shared_by_floor_and_cap(self):
        cap=render_mesh('cap_print();')
        floor=render_mesh('main_box();')
        x0,x1,y1,r=evaluate('[box_x0,box_x1,top_y1,corner_r]')
        for edge,direction in [(0,1),(x1-x0,-1)]:
            for angle in [2,5,10,20,30,40,45,50,60,70,80,85,88]:
                a=math.radians(angle)
                # 整个90°角都须沿R3圆弧，不能用直切角替代前半弧。
                for delta,solid in [(-.02,True),(.02,False)]:
                    x=edge+direction*(r-(r+delta)*math.cos(a))
                    depth=r-(r+delta)*math.sin(a)
                    self.assertEqual(contains(cap,(x,5,depth)),solid,msg=str((edge,angle,delta)))
                    self.assertEqual(contains(floor,(x+x0,y1-depth,1)),solid)

    def test_cap_corner_mesh_follows_r3_without_a_straight_chamfer(self):
        mesh=render_mesh('cap_print();')
        width,r=evaluate('[box_l,corner_r]')
        def section_bounds(z):
            xs=[]
            for tri in mesh:
                points=[]
                for a,b in zip(tri,tri[1:]+tri[:1]):
                    if (a[2]-z)*(b[2]-z)<0:
                        f=(z-a[2])/(b[2]-a[2])
                        points.append(tuple(a[i]+f*(b[i]-a[i]) for i in range(3)))
                if len(points)==2:
                    a,b=points
                    if (a[1]-5)*(b[1]-5)<0:
                        xs.append(a[0]+(5-a[1])*(b[0]-a[0])/(b[1]-a[1]))
            self.assertTrue(xs)
            return min(xs),max(xs)
        # 检查实际三角网格的截面半径；64面圆的弦误差约0.0036mm。
        for height in [.005,.02,.05,.1,.2,.4,.6,.8,1,1.3,1.6,2,2.4,2.8,2.95,2.995]:
            left,right=section_bounds(height)
            for inset in [left,width-right]:
                radius=math.hypot(inset-r,height-r)
                self.assertAlmostEqual(radius,r,delta=.005,msg=str((height,inset)))

    def test_cap_print_contains_only_the_finished_cap(self):
        mesh=render_mesh('cap_print();')
        validate_mesh(mesh,expected_components=1)
        product=render_mesh('translate([-box_x0,-floor_t,top_y1]) rotate([-90,0,0]) cap_body();')
        self.assertEqual(mesh,product)
        for x in [50.25,57.45,64.65,71.85]:
            self.assertFalse(contains(mesh,(x,25.5,10)))
        self.assertFalse(contains(mesh,(62.2,24.7,19.7)))

    def test_cap_fasteners_are_spread_across_the_available_bay(self):
        xs=evaluate('cap_fix_xs')
        self.assertGreaterEqual(xs[1]-xs[0],60)
        for mode in ['ceramic','patch','none']:
            definitions=[f'gps_mode="{mode}"']
            for x in xs:
                for advance in [0,2,4,8]:
                    body=f'''intersection(){{union(){{main_box();cap_body();gps_hardware();}}
                        translate([{x},cap_fix_y-{advance},cap_nut_z+.1])
                            cylinder(d=5/cos(30),h=2,$fn=6);}}'''
                    self.assertAlmostEqual(signed_volume(render_mesh(body,definitions,allow_empty=True)),0,places=6)

    def test_upper_glass_corners_restore_the_original_round_opening(self):
        box=render_mesh('main_box();')
        x0,x1,y1,z,r=evaluate('[cav_x0,cav_x1,cav_y1,z_glass_b+.5,glass_r+fit_glass]')
        for edge,direction in [(x0,1),(x1,-1)]:
            for angle in [20,45,70]:
                a=math.radians(angle)
                # 原玻璃内口R2.75：圆弧外侧为壳料，内侧必须为空。
                for delta,solid in [(.15,True),(-.15,False)]:
                    p=(edge+direction*(r-(r+delta)*math.cos(a)),y1-r+(r+delta)*math.sin(a),z)
                    self.assertEqual(contains(box,p),solid,msg=str(p))
        # 玻璃R2.5、单边0.25间隙；沿各轴移至公差边界仍能垂直落入。
        for dx,dy in [(0,0),(.25,0),(-.25,0),(0,.25),(0,-.25)]:
            body=f'''intersection(){{union(){{main_box();cap_body();}}
                translate([gx0+{dx},gy0+{dy},z_glass_b])
                    linear_extrude(glass_t+2)rrect(glass_l,glass_w,glass_r);}}'''
            self.assertAlmostEqual(signed_volume(render_mesh(body,allow_empty=True)),0,places=6)

    def test_speaker_removes_entire_obstructed_vent_without_short_stubs(self):
        box=render_mesh('main_box();')
        for y in [10.5,15,19,49,49.5]:
            self.assertTrue(contains(box,(84,y,.6)),msg=f'原散热槽残段 y={y}')
            self.assertFalse(contains(box,(70,y,.6)))
        disabled=render_mesh('main_box();',['speaker_enabled=false'])
        for y in [15,34,49]:self.assertFalse(contains(disabled,(84,y,.6)))

    def test_tray_has_only_center_notch_in_original_wire_edge(self):
        tray=render_mesh('gps_tray_body();')
        # 原Z+走线边两端完整，中央X14..20开到底板上表面Y73.19，保留1.6厚底板。
        for x in [7,12,13.98,14.02,17,19.98,20.02,22,27]:
            for y in [71.61,72.4,73.17]:
                self.assertTrue(contains(tray,(x,y,25.5)))
            for y in [73.21,74,77.42,79]:
                self.assertEqual(contains(tray,(x,y,25.5)),x<14 or x>20)
        for x in [5.8,28.2]:
            for z in [12,14.2,16.4]:
                for y in [72.4,77.42,79]:
                    self.assertTrue(contains(tray,(x,y,z)))
        self.assertFalse(contains(tray,(17,72.4,14.2))) # 背窗不变

    def test_all_labels_cut_eight_tenths_and_leave_backing(self):
        # 延伸实际字形，使用固定深度薄片检查槽底和余壁；字号/方向另有独立对照。
        cases=[('main_box();','wall_text(lbl_pwr,sw3_y,sw3_lbl_z,1);','box_x1',0),
               ('main_box();','wall_text(lbl_usb,h1_y,usb_lbl_z,1);','box_x1',0),
               ('cap_body();','wall_text(lbl_sd,sd_cx,sd_lbl_z,0);','box_y1',1)]
        cases += [('cap_body();',f'cap_label({i});','top_y1',1) for i in range(3)]
        for model,cutter,surface,axis in cases:
            modes=['ceramic','patch','none'] if cutter.startswith('cap_label') else ['ceramic']
            for mode in modes:
                for depth,removed in [(.7,True),(.9,False),(1.7,False)]:
                    with self.subTest(label=cutter,depth=depth,mode=mode):
                        shift='[-1,0,0]' if axis==0 else '[0,-1,0]'
                        pos=f'[{surface}-{depth}-.05,-100,-100]' if axis==0 else f'[-100,{surface}-{depth}-.05,-100]'
                        size='[.1,300,300]' if axis==0 else '[300,.1,300]'
                        sample=f'intersection(){{union(){{{cutter}translate({shift}){{{cutter}}}}}translate({pos})cube({size});}}'
                        v=signed_volume(render_mesh(sample))
                        self.assertGreater(v,.01,msg=cutter)
                        hit=signed_volume(render_mesh('intersection(){'+model+sample+'}',
                            ['labels_enabled=true',f'gps_mode="{mode}"'],allow_empty=True))
                        self.assertAlmostEqual(hit,0 if removed else v,places=4,msg=f'{cutter} depth={depth}')

    def test_usb_and_pwr_keep_the_accepted_two_point_five_mm_font(self):
        for txt,u in [('USB','h1_y'),('PWR','sw3_y')]:
            with self.subTest(label=txt):
                actual=f'wall_text("{txt}",{u},side_lbl_z,1);'
                expected=f'translate([box_x1-.8,{u},9.4])rotate([90,0,90])linear_extrude(.9)text("{txt}",size=2.5,font="Liberation Sans:style=Bold",halign="center",valign="center");'
                for first,second in [(actual,expected),(expected,actual)]:
                    self.assertAlmostEqual(signed_volume(render_mesh('difference(){'+first+second+'}',allow_empty=True)),0,places=5)

    def test_gps_keeps_original_position_and_size_and_top_text_faces_screen(self):
        # 字脚朝屏幕Z+：局部字的Y+映射为世界Z-，不能只看平放后的镜像预览。
        for name,actual,x,y,z in [
                ('GPS','cap_label(2);','90.5','top_y1','14.575+3.3+1.25+.3'),
                ('SD','wall_text(lbl_sd,sd_cx,sd_lbl_z,0);','sd_cx','box_y1','27.1')]:
            with self.subTest(label=name):
                expected=f'translate([{x},{y}-.8,{z}])rotate([-90,0,0])linear_extrude(.9)text("{name}",size=2.5,font="Liberation Sans:style=Bold",halign="center",valign="center");'
                for first,second in [(actual,expected),(expected,actual)]:
                    self.assertAlmostEqual(signed_volume(render_mesh('difference(){'+first+second+'}',allow_empty=True)),0,places=5)

    def test_speaker_pocket_accepts_square_body_and_keeps_floor_below_frame(self):
        box=render_mesh('main_box();')
        # 用户给定26方形完整外廓，胶层0.3；放入后不得碰浅仓壁/网格。
        speaker='translate([77,21,1.5])cube([26,26,4.3]);'
        self.assertAlmostEqual(signed_volume(render_mesh('intersection(){main_box();'+speaker+'}',allow_empty=True)),0,places=6)
        # 方框四个尖角也应有装配余量；外廓每边扩大0.2后仍须装入，不能只验名义26方框。
        clearance='translate([76.8,20.8,1.5])cube([26.4,26.4,4.3]);'
        self.assertAlmostEqual(signed_volume(render_mesh('intersection(){main_box();'+clearance+'}',allow_empty=True)),0,places=6)
        for x,y in [(77.5,21.5),(102.5,21.5),(77.5,46.5),(102.5,46.5)]:
            self.assertTrue(contains(box,(x,y,1.1)))
            self.assertFalse(contains(box,(x,y,1.3)))
        # 原x84贯通条栅在喇叭外框下必须封回，不能把它误当1.2mm余底。
        for y in [21.5,46.5]:
            self.assertTrue(contains(box,(84,y,.6)))
        for x,y in [(76.5,21),(103.5,21),(77,20.5),(77,47.5)]:
            self.assertTrue(contains(box,(x,y,2.3)))

    def test_speaker_grille_and_open_wire_recess_are_real_and_option_is_reversible(self):
        box=render_mesh('main_box();')
        for x,y in [(90,34),(86.4,34),(90,37.6)]:
            for z in [.1,.6,1.1]:self.assertFalse(contains(box,(x,y,z)))
        self.assertTrue(contains(box,(91.8,34,.6)))
        # 胶贴在振膜侧的外围：11×17区域以外保留底面，不开旧圆形20mm格栅。
        for x,y in [(97.2,34),(82.8,34),(90,44.8)]:
            self.assertTrue(contains(box,(x,y,.6)))
        self.assertTrue(contains(box,(93.6,41.2,.6))) # 最大圆角下的振膜角外仍保留胶面
        self.assertFalse(contains(box,(104.5,34,1.3)))
        self.assertTrue(contains(box,(104.5,34,.6)))
        disabled=render_mesh('main_box();',['speaker_enabled=false'])
        self.assertTrue(contains(disabled,(90,34,1.5)))
        self.assertFalse(contains(disabled,(84,25,.6)))

    def test_speaker_and_its_drop_in_path_clear_board_bosses_and_left_battery(self):
        # 板底13.4，喇叭含胶顶5.8；安装应在装板前从敞口垂直放入。
        for z in [1.5,3,7,15,30]:
            body=f'intersection(){{main_box();translate([77,21,{z}])cube([26,26,4.3]);}}'
            self.assertAlmostEqual(signed_volume(render_mesh(body,allow_empty=True)),0,places=6)
        obstacles='union(){translate([exp_x(0),exp_y(exp_w),z_exp_bot])cube([exp_l,exp_w,exp_t]);translate([9.75,3,floor_t])cube([65,54,9.5]);}'
        body='intersection(){translate([77,21,1.5])cube([26,26,4.3]);'+obstacles+'}'
        self.assertAlmostEqual(signed_volume(render_mesh(body,allow_empty=True)),0,places=6)

    def test_speaker_rejects_thin_floor_boss_overlap_and_oversize_grille(self):
        for definitions in [['speaker_recess=1.5'],['speaker_xy=[97.25,55]'],
                            ['speaker_diaphragm=[25,25]'],['speaker_wire_offset=13'],['speaker_size=[26,26,12]']]:
            with self.assertRaises(AssertionError,msg=str(definitions)):
                evaluate('speaker_z',definitions)

    def test_tray_has_two_diagonal_triangles_with_24mm_clearance(self):
        for height in [4.6,6.6]:
            tray=render_mesh('gps_tray_body();',[f'gps_ceramic_h={height}'])
            # 在原有坐标中交换到X/Z同号的另一组对角，不能只改预览方向。
            for x,z,present in [(-9,9,False),(9,-9,False),(-9,-9,True),(9,9,True)]:
                self.assertEqual(contains(tray,(17+x,74.89,14.2+z)),present)
            # 沿45度对角线在两侧切面内外探测，净距24而非边长24。
            for sign in [-1,1]:
                for radius,present in [(11.98,False),(12.02,True)]:
                    d=sign*radius/math.sqrt(2)
                    self.assertEqual(contains(tray,(17+d,74.89,14.2+d)),present)

    def test_chamfered_antenna_insertion_clears_tray_but_wrong_quarter_turn_hits(self):
        leg=20-24/math.sqrt(2)
        outline=[[-10+leg,-10],[10,-10],[10,10-leg],[10-leg,10],[-10,10],[-10,-10+leg]]
        for angle in [0,90]:
            body=f'''intersection(){{gps_tray_body();
                translate([17,81,14.2])rotate([90,0,0])linear_extrude(7.7)
                    rotate({angle})polygon({json.dumps(outline)});}}'''
            volume=signed_volume(render_mesh(body,allow_empty=True))
            if angle==0:self.assertAlmostEqual(volume,0,places=6)
            else:self.assertGreater(volume,1)

    def test_tray_rejects_corner_faces_that_cannot_fit_board(self):
        for gap in [0,29]:
            with self.assertRaises(AssertionError):
                evaluate('gps_corner_diag_clear',[f'gps_corner_diag_clear={gap}'])

    def test_ceramic_tray_reduced_upper_edge_accepts_connector(self):
        # 仅验证缩小后原出线口的4×4×2.5接头通道，不能借用其他侧墙。
        for height in [4.6,6.6]:
            definitions=[f'gps_ceramic_h={height}']
            info=validate_mesh(render_mesh('gps_tray_print();',definitions),expected_components=1)
            self.assertAlmostEqual(info['bounds_mm'][0][2],0,places=5)
            for advance in [0,4,8,12,16]:
                body=f'''intersection(){{gps_tray_body();
                    translate([gps_cx-2,gps_face_y-2.7,gps_cz+{advance}-2])cube([4,2.5,4]);}}'''
                self.assertAlmostEqual(signed_volume(render_mesh(body,definitions,allow_empty=True)),0,places=6)

    def test_ceramic_tray_has_lateral_stops_and_foam_seats(self):
        for dx in [-.35,.35]:
            mesh=render_mesh(f'intersection(){{main_box();translate([{dx},0,0])gps_tray_body();}}',allow_empty=True)
            self.assertGreater(signed_volume(mesh),.1)
        for dy in [-.4,.4]:
            mesh=render_mesh(f'intersection(){{main_box();translate([0,{dy},0])gps_tray_body();}}',allow_empty=True)
            self.assertGreater(signed_volume(mesh),.1)
        tray=render_mesh('gps_tray_body();')
        for x in [8.5,25.5]:
            self.assertTrue(contains(tray,(x,74,25.5)))
        for part in ['main_box();','cap_body();','gps_hardware();']:
            mesh=render_mesh('intersection(){gps_foam_envelope();'+part+'}',allow_empty=True)
            self.assertAlmostEqual(signed_volume(mesh),0,places=6)
        self.assertAlmostEqual(evaluate('gps_foam_gap'),.6,places=5)

    def test_sw3_flange_sits_inside_pocket_and_round_shoulder_remains(self):
        # 实测腔体为空、外周仍包护、外侧承拉肩0.8mm；不能仅改参数称为嵌入。
        pocket='translate([105.26,sw3_y,sw3_z])rotate([0,90,0])cylinder(d=6.4,h=2.18);'
        self.assertAlmostEqual(signed_volume(render_mesh('intersection(){main_box();'+pocket+'}',allow_empty=True)),0,places=6)
        for yoff in [-3.7,3.5]:
            probe=f'translate([105.26,sw3_y+{yoff},sw3_z-.1])cube([2.18,.2,.2]);'
            self.assertAlmostEqual(signed_volume(render_mesh('difference(){'+probe+'main_box();}',allow_empty=True)),0,places=6)
        shoulder='translate([107.46,sw3_y+2.3,sw3_z-.1])cube([.78,.4,.2]);'
        self.assertAlmostEqual(signed_volume(render_mesh('difference(){'+shoulder+'main_box();}',allow_empty=True)),0,places=6)
        # 同轴静位及工作全程，1mm厚法兰均在1.2mm凸环和墙内沉腔中。
        for stroke in [0,.2,.468,.668,.868,1.068]:
            flange=f'translate([sw3_tip_x+sw3_nose-{stroke},sw3_y,sw3_z])rotate([0,90,0])cylinder(d=pl_flange_d,h=pl_flange_t);'
            cavity='translate([105.25,sw3_y,sw3_z])rotate([0,90,0])cylinder(d=6.6,h=2.2);'
            self.assertAlmostEqual(signed_volume(render_mesh('difference(){'+flange+cavity+'}',allow_empty=True)),0,places=6)

    def test_sw3_fixed_inner_ring_is_only_one_point_two_mm(self):
        mesh=render_mesh('btn_guides();')
        xs=[p[0] for t in mesh for p in t]
        self.assertAlmostEqual(min(xs),105.25,places=5)
        self.assertEqual(evaluate('[sw3_guide,sw3_bore_in,sw3_nose]'),[1.2,-1,3.8])

    def test_rear_gps_cable_reaches_main_bay(self):
        # 1.6mm线包络：从背窗后侧经X28进入主仓。
        paths=['[[gps_cx,gps_tray_back_y-.8,gps_cz],[28,gps_tray_back_y-.8,gps_cz],[28,60,coax_z]]']
        for path in paths:
            body=f'''intersection(){{union(){{main_box();cap_body();gps_tray_body();}}
                let(path={path})for(i=[0:len(path)-2])hull(){{
                    translate(path[i])sphere(d=1.6,$fn=24);translate(path[i+1])sphere(d=1.6,$fn=24);}}}}'''
            self.assertAlmostEqual(signed_volume(render_mesh(body,allow_empty=True)),0,places=6)

    def test_recessed_flange_and_shaft_jointly_limit_tilt_and_reach_button(self):
        self.assertAlmostEqual(evaluate('sw3_work_stroke'),1.068,places=5)
        for stroke in [0,.468,.668,1.068]:
            span=1.8+stroke # 0.8mm杆孔＋1mm法兰＋推进量，非旧短环2.5mm杆孔。
            bound=min(math.degrees(math.asin(min(1,stroke/3.1))),math.degrees(math.atan(.4/span)))
            angle=.8*bound # 圆柱斜截面仍需留余量，实际实体交集再确认此姿态能发生。
            poses=[(a,-span*math.tan(math.radians(a))/2) for a in [-angle,angle]]+[(0,-.19),(0,.19)]
            for angle,offset in poses:
                moved=f"""module moved(){{translate([sw3_tip_x+sw3_nose-{stroke},sw3_y,sw3_z+{offset}])
                    rotate([0,-{angle},0])translate([-sw3_nose,0,0])rotate([0,90,0])plunger();}}"""
                for body in ['main_box();','translate([exp_x(0)+.2,exp_y(exp_w),z_exp_bot])cube([exp_l,exp_w,exp_t]);']:
                    self.assertAlmostEqual(signed_volume(render_mesh(moved+'intersection(){moved();'+body+'}',allow_empty=True)),0,places=6)
                if stroke==1.068:
                    body=moved+'intersection(){moved();translate([sw3_key_x-sw3_fit-sw3_travel,sw3_y-.8,z_exp_top+.5])cube([.6,1.6,.9]);}'
                    self.assertGreater(signed_volume(render_mesh(body,allow_empty=True)),0)
        # 内侧装入可逆；外肩防止向外脱出。未装板时不宣称双向锁死。
        for advance in [-4,-2,0]:
            inward=f'intersection(){{main_box();translate([sw3_tip_x+{advance},sw3_y,sw3_z])rotate([0,90,0])plunger();}}'
            self.assertAlmostEqual(signed_volume(render_mesh(inward,allow_empty=True)),0,places=6)
        outward='intersection(){main_box();translate([sw3_tip_x+.2,sw3_y,sw3_z])rotate([0,90,0])plunger();}'
        self.assertGreater(signed_volume(render_mesh(outward,allow_empty=True)),.1)

    def test_sw3_nose_clears_pcb_during_actual_switch_working_stroke(self):
        for travel in [.668,1.068]:
            for shift in [-.2,0,.2]:
                body=f"""intersection(){{
                    translate([sw3_tip_x-{travel},sw3_y,sw3_z]) rotate([0,90,0]) plunger();
                    translate([exp_x(0)+{shift},exp_y(exp_w),z_exp_bot]) cube([exp_l,exp_w,exp_t]);
                }}"""
                self.assertAlmostEqual(signed_volume(render_mesh(body,allow_empty=True)),0,places=6)
        # SW3 TS24CA F.Fab座子X[-.65,1.6]、Y±2.35，PCB实际板面链。
        for shift in [-.2,0,.2]:
            body=f"""intersection(){{
                translate([sw3_tip_x-sw3_work_stroke,sw3_y,sw3_z]) rotate([0,90,0]) plunger();
                translate([exp_x(sw3_pcb[0])-.65+{shift},sw3_y-2.35,z_exp_top]) cube([2.25,4.7,1.9]);
            }}"""
            self.assertAlmostEqual(signed_volume(render_mesh(body,allow_empty=True)),0,places=6)
        body="intersection(){btn_guides();translate([exp_x(0)+.2,exp_y(exp_w),z_exp_bot])cube([exp_l,exp_w,exp_t]);}"
        self.assertAlmostEqual(signed_volume(render_mesh(body,allow_empty=True)),0,places=6)
        for travel in [0,.668,1.068]:
            body=f"intersection(){{main_box();translate([sw3_tip_x-{travel},sw3_y,sw3_z])rotate([0,90,0])plunger();}}"
            self.assertAlmostEqual(signed_volume(render_mesh(body,allow_empty=True)),0,places=6)
        with self.assertRaises(AssertionError):evaluate('sw3_nose_d',['sw3_nose_d=2'])
        pins=render_mesh('plunger_plate();')
        self.assertTrue(contains(pins,(1.5,0,.1)))
        self.assertFalse(contains(pins,(1.5,0,7.1)))

    def test_raised_corner_tails_follow_a_smooth_radius_ramp(self):
        # 独立按平滑半径轨迹取内外表面探针，既拒绝方口，也拒绝直接挖空过渡区。
        root,box_height,radius=evaluate('[cap_root_y,box_h,corner_r]')
        for mode,height in [('ceramic',6.6),('ceramic',4.6),('patch',6.6),('none',6.6)]:
            inner=[];outer=[]
            corners=[(72.5,1,29.15,root),(108.25,-1,29.15,root)]
            if mode=='ceramic':corners += [(3,1,28,root+box_height-28),(31,-1,28,root+box_height-28)]
            for edge,direction,top,start in corners:
                for step in range(4,15):
                    y=start+step*.5
                    u=min(1,(y-start)/(2*radius))
                    r=radius*u*u*(3-2*u)
                    ztop=top if top<box_height else min(top,box_height+max(0,y-65.79))
                    for angle in [15,45,75]:
                        angle=math.radians(angle)
                        for target,offset in [(inner,-.10),(outer,.10)]:
                            q=r+offset
                            x=edge+direction*(r-q*math.cos(angle))
                            z=ztop-r+q*math.sin(angle)
                            target.append(f'translate([{x-.01},{y-.01},{z-.01}])cube([.02,.02,.02]);')
            defs=[f'gps_mode="{mode}"',f'gps_ceramic_h={height}']
            missing=render_mesh('difference(){union(){'+''.join(inner)+'}cap_body();}',defs,allow_empty=True)
            excess=render_mesh('intersection(){union(){'+''.join(outer)+'}cap_body();}',defs,allow_empty=True)
            self.assertAlmostEqual(signed_volume(missing),0,places=7,msg=f'内弧缺肉 {mode}/{height}')
            self.assertAlmostEqual(signed_volume(excess),0,places=7,msg=f'外弧仍有方角 {mode}/{height}')

    def test_raised_corner_roofs_keep_material_across_sloping_side_seams(self):
        # 在相邻45°坡面1.4mm壁厚内留两端0.1mm，沿整条接缝检查1.2mm连续肉。旧圆角切断此连接，
        # STL仍闭合，但从圆角上方可直接进入相邻屋面下面的舱内。
        for mode,height in [('ceramic',6.6),('ceramic',4.6),('patch',6.6),('none',6.6)]:
            probes="""union(){
                if(gps_mode=="ceramic") for(y=[65.2:.2:68.8])
                    for(x=[gps_bay_x0+.05,gps_bay_x1-.15])
                        translate([x,y,box_h-(y-cap_root_y)-top_roof_t+.1])cube([.1,.08,top_roof_t-.2]);
                // GPS高台左侧邻接坡面、右侧外墙；含支墩避让结束后的区域。
                for(y=[69.1:.2:71.5]) for(x=[cap_high_x+.05,box_x1-.15])
                    translate([x,y,box_h-(y-cap_root_y)-top_roof_t+.1])cube([.1,.08,top_roof_t-.2]);
            }"""
            missing=render_mesh('difference(){'+probes+'cap_body();}',
                                [f'gps_mode="{mode}"',f'gps_ceramic_h={height}'],allow_empty=True)
            self.assertAlmostEqual(signed_volume(missing),0,places=7,msg=f'{mode}/{height}')

    def test_exposed_raised_steps_have_real_xz_corner_radii(self):
        mesh=render_mesh('cap_body();')
        for x,z in [(2.6,27.8),(3.2,27.8),(30.8,27.8),(31.4,27.8),(72.05,28.95),(72.7,28.95),(108.05,28.95)]:
            body=f"intersection(){{cap_body();translate([{x},84,{z}]) cube([.05,.05,.05]);}}"
            self.assertAlmostEqual(signed_volume(render_mesh(body,allow_empty=True)),0,places=8,msg=str((x,z)))
        self.assertTrue(contains(mesh,(75.5,84,28.85)))
        self.assertTrue(contains(mesh,(17,84,27.8)))

    def test_compact_depth_and_centered_gps_mounting_face(self):
        self.assertEqual(evaluate('top_depth'),20)
        self.assertEqual(evaluate('[cap_high_h,sma_ports[2][0],sma_ports[2][1]]'),[29.15,90.5,14.575])
        cap=render_mesh('cap_body();')
        self.assertAlmostEqual(max(p[2] for tri in cap for p in tri),29.15,places=5)
        self.assertGreaterEqual(evaluate('gps_tray_back_y-exp_y(0)'),10)
        # 外部φ30包络允许上下各.425外伸，不能为完整圆盘抬高外壳。
        self.assertAlmostEqual(evaluate('sma_ports[2][2]/2-box_h/2'),.425,places=5)
        # φ18仅是此模型的中央实面承托检查，不代表通用天线底座规格。
        # 另外检查盖+底板的φ28.9周边，确实把底板厚度计入总承托高度。
        assembly=render_mesh('union(){main_box();cap_body();}')
        for i in range(72):
            angle=math.radians(i*5)
            for mesh,radius in [(cap,9),(assembly,14.45)]:
                self.assertTrue(contains(mesh,(90.5+radius*math.cos(angle),85.48,14.575+radius*math.sin(angle))),i)

    def test_sd_facade_has_no_added_diagonal_ribs(self):
        # 门面仍接低屋面，但不再增加用户不需要的两条长斜肋。
        for x in [40,61]:
            probe=f'translate([{x},68,18])cube([.5,.5,.5]);'
            self.assertAlmostEqual(signed_volume(render_mesh('intersection(){cap_body();'+probe+'}',allow_empty=True)),0,places=6)

    def test_sma_hex_pockets_fit_actual_nut_and_block_rotation(self):
        self.assertEqual(evaluate('[sma_nut_af,sma_nut_fit,sma_nut_depth]'),[8,.3,2.2])
        for i in range(3):
            for angle,clear in [(0,True),(30,False)]:
                body=f"""intersection(){{cap_body();let(p=sma_ports[{i}])
                    translate([p[0],top_inner_y1-sma_nut_depth+.1,p[1]])
                    rotate([-90,0,0]) rotate([0,0,{angle}]) cylinder(d=8/cos(30),h=2,$fn=6);}}"""
                volume=signed_volume(render_mesh(body,allow_empty=True))
                if clear:self.assertAlmostEqual(volume,0,places=6)
                else:self.assertGreater(volume,.1)
        cap=render_mesh('cap_body();')
        for x,z in [(50.25,7.5),(68.25,7.5),(90.5,14.575)]:
            self.assertFalse(contains(cap,(x+4,82.5,z))) # 六角槽内部
            self.assertTrue(contains(cap,(x+4,83.8,z)))  # 原1.8mm穿板承压面
            self.assertTrue(contains(cap,(x+5,81.6,z)))  # 内增2.2mm止转肉
            self.assertFalse(contains(cap,(x+5,81.4,z))) # 槽座后方开放装螺母

    def test_reworked_main_wall_is_open_except_grounded_support_islands(self):
        mesh = render_mesh('main_box();')
        for x in [25, 34, 67, 83, 96]:
            for z in [8, 18.5, 27]:
                self.assertFalse(contains(mesh, (x, 64.4, z)), (x,z))
        for x in [12, 75]:
            for z in [4, 12, 27]:
                self.assertTrue(contains(mesh, (x, 64.4, z)), (x,z))

    def test_cap_captures_ceramic_tray_upper_edge_without_foam(self):
        for height in [4.6,6.6]:
            defs=[f'gps_ceramic_h={height}']
            for dx in [-.4,.4]:
                body=f'intersection(){{cap_body();translate([{dx},0,0])gps_tray_body();}}'
                self.assertGreater(signed_volume(render_mesh(body,defs,allow_empty=True)),.05)
            for angle in [-2,2]:
                body=f'''intersection(){{cap_body();translate([gps_cx,0,floor_t])
                    rotate([0,{angle},0])translate([-gps_cx,0,-floor_t])gps_tray_body();}}'''
                self.assertGreater(signed_volume(render_mesh(body,defs,allow_empty=True)),.01)
                body=f'''intersection(){{cap_body();translate([0,(gps_tray_back_y+gps_face_y)/2,floor_t])
                    rotate([{angle},0,0])translate([0,-(gps_tray_back_y+gps_face_y)/2,-floor_t])gps_tray_body();}}'''
                self.assertGreater(signed_volume(render_mesh(body,defs,allow_empty=True)),.01)
            # 前后均有上挡，不能只防左右倾倒而让托盘朝主仓翻倒。
            body='intersection(){cap_body();translate([0,.4,0])gps_tray_body();}'
            self.assertGreater(signed_volume(render_mesh(body,defs,allow_empty=True)),.05)
            body='intersection(){cap_body();translate([0,-.4,0])gps_tray_body();}'
            self.assertGreater(signed_volume(render_mesh(body,defs,allow_empty=True)),.05)

    def test_sd_facade_belongs_to_cap_and_leaves_base_open(self):
        box=render_mesh('main_box();')
        cap=render_mesh('cap_body();')
        for x,z in [(40,27),(61,27),(50.8,27)]:
            self.assertFalse(contains(box,(x,64.4,z)))
            self.assertTrue(contains(cap,(x,64.4,z)))
        for x in [40,50.8,61]:
            self.assertFalse(contains(box,(x,64.4,8)))
            self.assertFalse(contains(cap,(x,64.4,8)))

    def test_power_side_glass_tab_aligns_with_bolt_and_is_thirty_percent_narrower(self):
        tabs=render_mesh('glass_tabs();')
        x,z,y,width=evaluate('[cav_x1-.5,z_glass_b-.2,boss_xy[1][1],tab_w*.7]')
        for yy in [y-width/2+.05,y,y+width/2-.05]:
            self.assertTrue(contains(tabs,(x,yy,z)))
        for yy in [y-width/2-.05,y+width/2+.05,12]:
            self.assertFalse(contains(tabs,(x,yy,z)))
        self.assertTrue(contains(tabs,(x,50,z)))

    def test_reworked_floor_has_one_rounded_outer_contour(self):
        mesh = render_mesh('main_box();')
        self.assertFalse(contains(mesh, (-10.1,85.3,1)))
        self.assertTrue(contains(mesh, (-9.3,64.9,1)))
        self.assertTrue(contains(mesh, (-7.3,82.5,1)))

    def test_reworked_roof_is_closed_and_post_has_printing_rib(self):
        mesh = render_mesh('cap_body();')
        for x in [34]:
            self.assertTrue(contains(mesh, (x,68,24.5)))
        for x in [83,96]:
            self.assertTrue(contains(mesh, (x,68,28.45)))
        for x in evaluate('cap_fix_xs'):
            for y in [79,80,81,82,83]:
                self.assertTrue(contains(mesh, (x,y,8)))

    def test_default_scene_is_all_print_parts_on_one_180mm_plate(self):
        with tempfile.TemporaryDirectory(prefix='case-v4-layout-') as tmp:
            for mode, count in [('ceramic',7),('patch',6),('none',6)]:
                out=Path(tmp)/f'{mode}.stl'
                scad_run(SCAD,out,[f'gps_mode="{mode}"'])
                mesh=triangles(out)
                info=validate_mesh(mesh, expected_components=count)
                self.assertLessEqual(max(info['size_mm'][:2]),180)
                self.assertAlmostEqual(info['size_mm'][0],118.5,places=3)
                bounds=component_bounds(mesh)
                self.assertEqual(len(bounds),count)
                for lower,upper in bounds:
                    self.assertAlmostEqual(lower[2],0,places=4)
                    self.assertGreaterEqual(round(min(lower[:2]),6),0)
                    self.assertLessEqual(max(upper[:2]),180)

    def test_print_alias_still_exports_only_box(self):
        with tempfile.TemporaryDirectory(prefix='case-v4-default-') as tmp:
            default, box = Path(tmp)/'default.stl', Path(tmp)/'box.stl'
            scad_run(SCAD, default, ['part="print"'])
            scad_run(SCAD, box, ['part="box"'])
            self.assertEqual(triangles(default), triangles(box))
            validate_mesh(triangles(default), expected_components=1)

    def test_assembly_reference_hardware_does_not_enter_stl(self):
        with tempfile.TemporaryDirectory(prefix='case-v4-reference-') as tmp:
            clean, reference = Path(tmp)/'clean.stl', Path(tmp)/'reference.stl'
            scad_run(SCAD, clean, ['part="assembly"', 'preview_hardware=false'])
            scad_run(SCAD, reference, ['part="assembly"', 'preview_hardware=true'])
            self.assertEqual(triangles(clean), triangles(reference))

    def test_all_internal_gps_modes_keep_three_independent_sma_positions(self):
        for mode in ['ceramic','patch','none']:
            self.assertEqual(evaluate('sma_ports',[f'gps_mode="{mode}"']), [[50.25,7.5,15],[68.25,7.5,15],[90.5,14.575,30]])

    def test_main_stack_and_usb_switch_positions_preserve_original(self):
        expr='[z_glass_b,z_scr_bot,z_exp_top,box_h,boss_xy,usb_y0,usb_y1,usb_z0,usb_z1,sw3_y,sw3_z,led_ys,led_z]'
        self.assertEqual(evaluate(expr),evaluate(expr,source=ORIGINAL))

    def test_ceramic_air_gap_and_thickness_change_actual_back_plane(self):
        base=evaluate('[gps_face_y,gps_back_y,gps_air_gap,top_y1-gps_ceramic_cap_t-gps_face_y]')
        thin=evaluate('[gps_face_y,gps_back_y]', ['gps_ceramic_h=4.6'])
        self.assertAlmostEqual(base[2],4)
        self.assertAlmostEqual(base[3],4)
        self.assertAlmostEqual(thin[0],base[0])
        self.assertAlmostEqual(thin[1]-base[1],2)
        self.assertGreaterEqual(evaluate('gps_tray_back_y-exp_y(0)'),10)

    def test_invalid_antenna_or_back_support_is_rejected(self):
        for definition in ['gps_air_gap=15','gps_back_component_size=[20,20,3]','sma_external_ds=[24,24,30]']:
            with self.assertRaises(AssertionError):evaluate('box_h',[definition])

    def test_default_15mm_margin_and_unsupported_extreme_18mm_is_rejected(self):
        default=evaluate('sma_pair_gaps')
        self.assertAlmostEqual(default[0],3)
        self.assertAlmostEqual(default[1],.847765,places=5)
        # 原总宽内保持贴片10mm和GPS外厚齐平，18mm组合不满足保守间距。
        with self.assertRaises(AssertionError):evaluate('box_h',['sma_external_ds=[18,18,30]'])

    def test_print_parts_export_closed_and_start_on_bed(self):
        with tempfile.TemporaryDirectory(prefix='case-v4-mesh-') as tmp:
            for mode, thickness in [('ceramic',6.6), ('ceramic',4.6), ('patch',6.6), ('none',6.6)]:
                parts = ['box','cap','gps_tray'] if mode=='ceramic' else ['box','cap']
                for part in parts:
                    with self.subTest(part=part, mode=mode, thickness=thickness):
                        path=Path(tmp)/f'{part}.stl'
                        scad_run(SCAD,path,[f'part="{part}"',f'gps_mode="{mode}"',f'gps_ceramic_h={thickness}'])
                        mesh=triangles(path)
                        validate_mesh(mesh,expected_components=1)
                        for lower,_ in component_bounds(mesh):
                            self.assertAlmostEqual(lower[2],0,places=4)

    def test_assembled_rigid_parts_have_no_volume_intersection(self):
        with tempfile.TemporaryDirectory(prefix='case-v4-intersections-') as tmp:
            src=Path(tmp)/'intersect.scad'
            src.write_text(f'include <{SCAD}>\nintersection() {{ main_box(); cap_body(); }}\n')
            # Empty STL is reported by OpenSCAD; a real nonempty intersection must fail this test.
            import subprocess
            from test_case import OPENSCAD
            out=Path(tmp)/'intersection.stl'
            r=subprocess.run([OPENSCAD,'--backend=Manifold','-D','part=""','-o',str(out),str(src)],capture_output=True,text=True)
            if 'Current top level object is empty' not in r.stderr:
                mesh=triangles(out)
                volume=sum(sum(a[i]*(b[(i+1)%3]*c[(i+2)%3]-b[(i+2)%3]*c[(i+1)%3]) for i in range(3))/6 for a,b,c in mesh)
                self.assertAlmostEqual(volume,0,places=5, msg='允许承压面接触，不允许正体积穿插')
            self.assertNotIn('WARNING:',r.stderr)
            self.assertNotIn('ERROR:',r.stderr)

    def test_tray_and_hardware_do_not_penetrate_assembled_plastic(self):
        pairs = [('main_box()', 'gps_tray_body()'), ('cap_body()', 'gps_tray_body()'),
                 ('gps_tray_body()', 'gps_hardware()'), ('cap_body()', 'gps_hardware()')]
        for thickness in [4.6, 6.6]:
            for left, right in pairs:
                with self.subTest(thickness=thickness, pair=(left, right)):
                    mesh = render_mesh(f'intersection() {{ {left}; {right}; }}',
                                       [f'gps_ceramic_h={thickness}'], allow_empty=True)
                    self.assertAlmostEqual(signed_volume(mesh), 0, places=5)

    def test_preview_sma_metal_and_fasteners_clear_all_printed_parts(self):
        for mode in ['none', 'patch', 'ceramic']:
            tray = 'gps_tray_body();' if mode=='ceramic' else ''
            mesh = render_mesh('intersection() { hardware_preview(); '
                               f'union() {{ main_box(); cap_body(); {tray} }} }}',
                               [f'gps_mode="{mode}"'], allow_empty=True)
            self.assertAlmostEqual(signed_volume(mesh), 0, places=5)

    def test_three_through_holes_are_present_in_every_gps_mode(self):
        for mode in ['none', 'patch', 'ceramic']:
            mesh = render_mesh('cap_body();', [f'gps_mode="{mode}"'])
            for x, z in [(50.25, 7.5), (68.25, 7.5), (90.5, 14.575)]:
                for y in [83.8, 84.5, 85.4]:
                    self.assertFalse(contains(mesh, (x, y, z)))
                    self.assertTrue(contains(mesh, (x + 3.8, y, z)))

    def test_ceramic_total_hardware_envelope_and_real_air_layer(self):
        for thickness in [4.6, 6.6]:
            definitions = [f'gps_ceramic_h={thickness}']
            mesh = render_mesh('gps_hardware();', definitions)
            ys = [p[1] for t in mesh for p in t]
            self.assertAlmostEqual(max(ys)-min(ys), thickness, places=4)
            self.assertAlmostEqual(max(ys), 80.29, places=4)
            cap = render_mesh('cap_body();', definitions)
            for x in [8, 17, 26]:
                for z in [5.2, 14.2, 23.2]:
                    for y in [80.5, 82, 84.2]:
                        self.assertFalse(contains(cap, (x, y, z)))
                    self.assertTrue(contains(cap, (x, 84.5, z)))
            tray = render_mesh('gps_tray_body();', definitions)
            pcb_back = 80.29-thickness+1.6
            for dx, dz, present in [(-8.5,-8.5,True), (-8.5,8.5,False), (8.5,-8.5,False), (8.5,8.5,True)]:
                self.assertEqual(contains(tray, (17+dx, pcb_back-.4, 14.2+dz)),present)
                self.assertFalse(contains(tray, (17+dx, pcb_back-.1, 14.2+dz)))
            for y in [78, pcb_back-1]:
                self.assertFalse(contains(tray, (17, y, 14.2)))

    def test_patch_window_is_point_eight_mm_not_a_hole(self):
        cap = render_mesh('cap_body();', ['gps_mode="patch"'])
        for x in [-7.24, 12.75, 32.74]:
            self.assertFalse(contains(cap, (x, 84.6, 7.5)))
            self.assertTrue(contains(cap, (x, 84.8, 7.5)))

    def test_continuous_coax_routes_clear_plastic_for_all_modes_and_adjustments(self):
        for mode in ['none','patch','ceramic']:
            for adjust in [-.5,0,.5]:
                definitions=[f'gps_mode="{mode}"',f'exp_support_adjust={adjust}']
                tray='gps_tray_body();' if mode=='ceramic' else ''
                body='intersection(){coax_envelope();union(){main_box();cap_body();'+tray+'}}'
                mesh=render_mesh(body,definitions,allow_empty=True)
                self.assertAlmostEqual(signed_volume(mesh),0,places=5,msg=str((mode,adjust)))

    def test_sd_card_sweep_and_open_finger_approach_are_clear(self):
        for body in ['main_box();','cap_body();']:
            mesh=render_mesh(body)
            for y in [64.4,66,75,90,100]:
                for x in [43,50.8,58.6]:
                    for z in [18.15,19.8,21.45]:
                        self.assertFalse(contains(mesh,(x,y,z)),(body,x,y,z))
            for y in [67,75,85]:
                self.assertFalse(contains(mesh,(50.8,y,25)))
        cap=render_mesh('cap_body();')
        self.assertFalse(contains(cap,(50.8,64.4,25.4)))
        self.assertTrue(contains(cap,(50.8,64.4,25.7)))
        self.assertTrue(contains(cap,(43,64.4,23)))

    def test_cap_can_be_lowered_vertically_over_islands_and_tray(self):
        for lift in [0,.3,1,3,8,20]:
            body=f'intersection(){{translate([0,0,{lift}]) cap_body();union(){{main_box();gps_tray_body();}}}}'
            self.assertAlmostEqual(signed_volume(render_mesh(body,allow_empty=True)),0,places=5)

    def test_gps_roof_stays_flush_past_glass_island(self):
        body="intersection(){cap_body();translate([71,cap_root_y,box_h+.001]) cube([8,box_y1+cap_fit-cap_root_y,5]);}"
        self.assertAlmostEqual(signed_volume(render_mesh(body,allow_empty=True)),0,places=6)
        cap=render_mesh('cap_body();')
        self.assertTrue(contains(cap,(83,65.5,28.5)))
        self.assertFalse(contains(cap,(83,65.5,29.3)))
        for y in [67,70,78,84]:
            self.assertTrue(contains(cap,(83,y,28.5)))
            self.assertFalse(contains(cap,(83,y,29.3)))

    def test_rear_joint_has_backing_but_does_not_touch_glass(self):
        cap=render_mesh('cap_body();')
        for x in [25,34,67,83,96]:
            self.assertTrue(contains(cap,(x,63.8,27.5)))
            self.assertFalse(contains(cap,(x,63.5,27.5)))
            self.assertFalse(contains(cap,(x,63.3,28.05)))
        box=render_mesh('main_box();')
        for x in [-7.5,105.5]:
            self.assertTrue(contains(box,(x,63.8,10)))

    def test_cap_can_lift_then_slide_out_with_glass_and_module_left_in_place(self):
        # 后挡捕获托盘：先抬2mm释放上沿，再平移。玻璃取最靠顶舱的公差位置。
        poses=[(0,z) for z in [0,.3,.6,1,1.4,2]]+[(y,2) for y in [.3,1,3,8,30]]
        for mode,height in [('ceramic',6.6),('ceramic',4.6),('patch',6.6),('none',6.6)]:
            tray='gps_tray_body();' if mode=='ceramic' else ''
            # 贴片粘在盖内侧，随盖移动；陶瓷模块留在底座托盘内。
            moving_hardware='gps_hardware();' if mode=='patch' else ''
            fixed_hardware='gps_hardware();' if mode=='ceramic' else ''
            for advance,lift in poses:
                body=f'''intersection() {{ translate([0,{advance},{lift}]) union(){{cap_body();{moving_hardware}}}
                    union() {{ main_box(); {tray} {fixed_hardware}
                        translate([gx0,gy0+fit_glass,z_glass_b])
                            linear_extrude(glass_t)rrect(glass_l,glass_w,glass_r); }} }}'''
                self.assertAlmostEqual(signed_volume(render_mesh(body,[f'gps_mode="{mode}"',f'gps_ceramic_h={height}'],allow_empty=True)),0,places=5,msg=str((mode,height,advance,lift)))

    def test_open_top_rejects_unsupported_legacy_y_buttons(self):
        with self.assertRaises(AssertionError):
            evaluate('box_h',['btns=[40]'])

    def test_m25_fasteners_have_nut_pockets_and_load_shoulders(self):
        box = render_mesh('main_box();')
        cap = render_mesh('cap_body();')
        for x in evaluate('cap_fix_xs'):
            for z in [1, 3]:
                self.assertFalse(contains(box, (x, 70.1, z)))
            self.assertFalse(contains(box, (x+2, 70.1, 1)))
            self.assertTrue(contains(box, (x+2, 70.1, 3)))
            self.assertFalse(contains(cap, (x, 70.1, 8)))
            self.assertTrue(contains(cap, (x+2, 70.1, 13)))
            self.assertFalse(contains(cap, (x+2, 70.1, 10)))
            self.assertFalse(contains(cap, (x,70.1,11.8))) # 螺钉端部余量
            self.assertTrue(contains(cap, (x,70.1,12.2))) # 盲孔封顶
            self.assertTrue(contains(cap, (x+2,70.1,8.8))) # 螺母下承拉肩
            self.assertFalse(contains(cap, (x,66.2,10))) # 从内部侧插可达
            for angle in range(0,360,30):
                a=math.radians(angle)
                self.assertTrue(contains(box,(x+3.9*math.cos(a),70.1+3.9*math.sin(a),3.5)))
                self.assertTrue(contains(cap,(x+3.9*math.cos(a),70.1+3.9*math.sin(a),5)))
            for advance in [0,2,4,8]:
                body=f"intersection(){{cap_body();translate([{x},cap_fix_y-{advance},cap_nut_z+.1]) cylinder(d=5/cos(30),h=2,$fn=6);}}"
                self.assertAlmostEqual(signed_volume(render_mesh(body,allow_empty=True)),0,places=6)
            for angle in range(0,360,30):
                a=math.radians(angle)
                self.assertFalse(contains(box,(x+2.7*math.cos(a),70.1+2.7*math.sin(a),.5)))
                self.assertTrue(contains(box,(x+3.2*math.cos(a),70.1+3.2*math.sin(a),.5)))

    def test_sma_labels_have_printable_size_and_clearance_beside_holes(self):
        for i,txt,x,z in [(0,'ADS-B',50.25,7.5),(1,'AIR',68.25,7.5)]:
            with self.subTest(label=i):
                mesh=render_mesh(f'cap_label({i});')
                xs=[p[0] for t in mesh for p in t];zs=[p[2] for t in mesh for p in t]
                self.assertAlmostEqual(max(xs),x-3.3-1.2,delta=.002) # 字体曲线离散化约0.001mm
                self.assertGreaterEqual(min(xs),[33.35+.6,50.25+3.3+.6][i])
                self.assertAlmostEqual((max(zs)+min(zs))/2,z,places=2)
                self.assertGreater(max(zs)-min(zs),2.3)
                self.assertLess(max(zs)-min(zs),2.5)
                # 独立预期字形：无横向压缩、字号2.5，字脚朝屏幕侧。
                expected=f'''let(m=textmetrics("{txt}",size=2.5,font="Liberation Sans:style=Bold",spacing=1.1))
                    translate([{x}-3.3-1.2,top_y1-.8,{z}])rotate([-90,0,0])linear_extrude(.9)
                    translate([-m.position.x-m.size.x,-m.position.y-m.size.y/2])
                    text("{txt}",size=2.5,font="Liberation Sans:style=Bold",spacing=1.1);'''
                actual=f'cap_label({i});'
                for first,second in [(actual,expected),(expected,actual)]:
                    self.assertAlmostEqual(signed_volume(render_mesh('difference(){'+first+second+'}',allow_empty=True)),0,places=5)

if __name__=='__main__':unittest.main()
