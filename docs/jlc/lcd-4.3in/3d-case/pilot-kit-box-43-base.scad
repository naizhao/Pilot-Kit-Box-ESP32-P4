// =============================================================
// Pilot Kit Box — 4.3" 触摸屏版 3D 打印底座（盒体）v2
// =============================================================
// 结构：单件薄壁盒体，全部自顶部装配（无底盖）。
//
// 紧固链（自下而上，底座不埋螺母）：
//   M2.5 螺丝从底面沉孔穿入 → 打印套筒柱（φ2.9 通孔）
//   → 扩展板安装孔 → 跨过 HAT 对接造成的悬空段（stack_h−铜柱长）
//   → 锁进挂在微雪板下的 3.8 铜柱。
//   堆叠实测：微雪板 → HAT母3.5+HAT公3.5（板间距 7.0）→ 扩展板；
//   铜柱 3.8 悬空 3.2。建议螺丝上套 3.2 隔套（或换 7mm 铜柱）再锁紧，
//   否则拧紧力矩会压在 HAT 排针上。
//
// 减料设计：
//   - 内腔按【玻璃轮廓】而非 PCB 轮廓，四周到板边 ≥3.8mm，
//     SD 卡座 / SMA 线柱等板边突出物不会顶墙
//   - 玻璃仅靠周圈薄支撑舌托住（螺丝已锁死整叠，只需扣住）
//   - 底板条栅镂空，侧壁 1.8；开孔处仅外沉 0.5、余厚 1.3
//
// 打印提示：底面 4 个外凸脚（免贴胶），意味着底面悬空 foot_h，
//   切片开「仅打印板支撑」或接受底面轻微垂丝（朝下不可见）。
//   沉孔配盘头/沉头 M2.5（圆柱头太高会露出底面）。
//
// 坐标系（俯视，屏朝上，原点 = 屏板 X-/Y- 角）：
//   X+ = USB 壁（H1 Type-C，玻璃悬出 3.7）
//   X- = SMA 壁（无板载元件，玻璃悬出 8.2，内侧留 8.6 给螺母/线柱）
//   Y+ = SD 卡槽 + RESET/BOOT/POWER 按键壁
//   Y- = 40PIN 边（不开孔）
//
// 尺寸来源：
//   - 微雪 dimensions.pdf（20260411）：玻璃 114.4×66.8，
//     PCB 102.5×60，孔距 92×50 M2.5，厚 11.15（玻璃 1.6/板下净空 4）
//   - 扩展板 docs/jlc/lcd-4.3in/pcb（实读）：100×60，
//     4 孔 φ2.6 @ (4,5)/(96,5)/(4,55)/(96,55)，孔距同 92×50
//
// 实测记录（2026-07-31 卡尺，一切以玻璃为基准）：
//   - H1：座顶距玻璃顶边 20 / 座底距底边 37 → 中心 y=38.5 ✓与推导吻合
//   - SD：右缘距玻璃右(H1侧) 45 / 左缘距左 53 → 中心=玻璃左+61.2，深 9.0
//   - POWER：中心距玻璃左 31，深 8.5；BOOT +39 / RESET +47
//   - 悬出方向实测定性确认：USB 侧小、C6 侧大（量值保持图纸链
//     3.7/8.2/3.4，总和闭合 114.4/66.8；卡尺读数 2/7/2 疑似量到
//     显示模组边框，每边系统性少 ~1.4）
//   （深度=玻璃外表面到器件中心）
// ⚠ 仍待实测：
//   1. glass_ov_xm/xp —— 玻璃悬出方向。一个量法定乾坤：H1 座端面
//      距玻璃右边缘的缩进 ≈2.4 → 当前假设对；≈6.9 → 对调两参数
//   2. stack_h —— 两板对插实际间距（= 铜柱长度）
//   3. sma_y / sma_z —— 面板 SMA 孔位（自定）
// =============================================================

/* ---------- 输出选择 ---------- */
part = "print";       // "print" 盒体+按键销连筋一体（下单用这个）
                      // "box" 仅盒体 / "plungers" 仅按键销 / "section" X 剖面
align_origin = true;  // true = 输出平移到包围盒角点(0,0,0)，方便量尺；
                      // 内部坐标系不变（原点=屏板 X-/Y- 角）

/* ---------- 打印公差 ---------- */
fit_glass = 0.4;      // 玻璃四周单边间隙
$fn = 64;

/* ---------- 屏组件（微雪板）---------- */
glass_l = 114.4;  glass_w = 66.88; glass_t = 1.0;  glass_r = 2.5;  // 玻璃实测 114.4×66.88，厚+背胶 1.0
scr_pcb_l = 102.5;  scr_pcb_w = 60.0;  scr_pcb_t = 1.6;
scr_under = 4.0;                       // 屏板下方元件净空
scr_glass_gap = 11.15 - glass_t - scr_pcb_t - scr_under;  // 3.95
glass_ov_xm = 8.2;    // 玻璃悬出：X-（非 USB 侧）
glass_ov_xp = 3.7;    // 玻璃悬出：X+（USB 侧）
glass_ov_y  = (glass_w - scr_pcb_w) / 2;                  // 3.4
// 方位约定（面朝屏幕，PCB 在后）：左=C6 侧(X-)，右=USB H1/H2 侧(X+)，
// 顶=SD/按键边(Y+)。开孔位一律以玻璃边缘实测值标注。
glass_left = -glass_ov_xm;     // 玻璃左边缘 x（内部坐标）

/* ---------- 扩展板 ---------- */
exp_t = 1.6;
hole_dx = 92;  hole_dy = 50;           // 安装孔距（两板一致）

/* ---------- 高度链（自内底面起）---------- */
floor_t = 2.4;        // 底板厚（含沉孔）
bat_h   = 11.0;       // 电池仓净高 = 套筒柱高（电池 9.5 + 余量 1.5，首装宽松）
stack_h    = 7.0;     // 实测：HAT 母 3.5 + HAT 公 3.5 对接 = 板间距 7.0
pillar_len = 3.8;     // 挂在微雪板下的铜柱长；悬空 = stack_h - pillar_len = 3.2

/* ---------- 壁与柱 ---------- */
wall_t     = 1.8;
boss_d     = 7.0;
screw_d    = 2.9;     // M2.5 通孔
cb_d       = 5.6;     // 底面沉孔（圆柱头/盘头）
cb_depth   = 1.4;
corner_r   = 3.0;

/* ---------- 玻璃支撑舌 ---------- */
tab_w = 8;            // 舌宽
tab_t = 1.5;          // 舌厚（垂直方向）
tab_p = 2.8;          // 自内壁面伸出量
tab_p_left = 5.0;     // 左(C6)侧加深：该侧玻璃悬出最大，多托一点

/* ---------- 电池 ----------
   955465 软包（65×54×9.5）自由平放，上被扩展板压住、周围是柱子，
   不做定位挡墙。注意躲开扩展板底面 x≥76 的 SDR 悬垂区。 */

/* ---------- USB H1（X+ 壁）---------- */
h1_y     = scr_pcb_w - 21.4;   // 中心 y=38.6
usb_w    = 12.0;  usb_h = 8.0; // 直插穿墙孔：公头包胶(宽11)整体穿入，
                               // 实测插满时包胶面与玻璃边平齐，故不做外沉槽
usb_glass_d = 9.0;             // 实测：玻璃面到孔顶边 5 / 底边 13 → 中心 9
usb_zoff = glass_t + scr_glass_gap + scr_pcb_t - usb_glass_d;  // -2.35
rec_depth = 0.5;               // SD 外沉深度（1.8 壁余厚 1.3）

/* ---------- 按键（Y+ 壁）：只开 POWER ---------- */
// 实测：中心=玻璃左+31(POWER) / +39(BOOT) / +47(RESET)
btn_power_x = glass_left + 31;     // 22.8，离 SD 最远
btns        = [btn_power_x];       // 需要 RESET 时加 glass_left+47
btn_d       = 4.1;                 // 导孔（配打印按压销）
btn_glass_d = 8.5;                 // 实测：玻璃面到键帽中心深度
btn_zoff    = glass_t + scr_glass_gap + scr_pcb_t - btn_glass_d;  // -1.35
btn_pkt_d   = 6.6;                 // 内壁沉窝：卡按压销法兰
btn_pkt_dp  = 0.8;
pl_shaft_d  = 3.7;                 // 按压销杆径
pl_flange_d = 6.2;  pl_flange_t = 2.5;
pl_nose     = 2.0;                 // 法兰前的 φ 杆径顶鼻。实测键帽面距玻璃边
                                   // 4.0 → 按压面停在其前 0.7，行程需求 ~0.95
pl_proud    = 2.0;                 // 静止时销尾高出外壁量
n_plungers  = 4;                   // 一体打印的按键销数量（1 用 2 备）

/* ---------- SD 卡槽（Y+ 壁）---------- */
sd_cx    = glass_left + 59;    // 50.8。实测：卡座中心=玻璃左+59
sd_w     = 16.0;  sd_h = 3.5;
sd_glass_d = 9.0;              // 实测：玻璃面到卡座中心深度
sd_zoff  = glass_t + scr_glass_gap + scr_pcb_t - sd_glass_d;      // -1.85
sd_rec   = [22, 8];            // 外侧圆角沉槽（深 rec_depth）
sd_notch = 10;                 // 指甲槽宽（圆角，贯穿余壁）

/* ---------- SMA ×2（X- 壁，面板式穿墙）---------- */
sma_y = [19, 41];     // 两孔中心 y
sma_z = 19;           // 孔中心高（自内部坐标系底面；盒子变矮后下调留口沿余量）
sma_d = 6.4;          // 面板 SMA 螺纹穿孔
sma_cb_d = 9.5;  sma_cb_dp = 0.6;   // 内侧沉窝：留平面好上螺母

/* ---------- 底面 ---------- */
foot_d = 8;  foot_h = 1.2;  foot_inset = 5.5;  // 外凸脚，高过螺丝头余量
lighten = true;       // 底板条栅镂空（6 条均布，兼顾透气与省料）
slat_w  = 4;
slat_xs = [14, 28, 42, 56, 70, 84];   // 关于板中心 x=51.25 大致对称
slat_y0 = 10;  slat_y1 = 50;

/* =============================================================
   派生尺寸（勿直接改）
   ============================================================= */
gx0 = -glass_ov_xm;             gx1 = scr_pcb_l + glass_ov_xp;
gy0 = -glass_ov_y;              gy1 = scr_pcb_w + glass_ov_y;

cav_x0 = gx0 - fit_glass;  cav_x1 = gx1 + fit_glass;   // 内腔=玻璃轮廓
cav_y0 = gy0 - fit_glass;  cav_y1 = gy1 + fit_glass;

box_x0 = cav_x0 - wall_t;  box_x1 = cav_x1 + wall_t;
box_y0 = cav_y0 - wall_t;  box_y1 = cav_y1 + wall_t;
box_l = box_x1 - box_x0;   box_w = box_y1 - box_y0;

z_exp_bot = floor_t + bat_h;
z_exp_top = z_exp_bot + exp_t;
z_scr_bot = z_exp_top + stack_h;          // 屏板底面
z_glass_b = z_scr_bot + scr_pcb_t + scr_glass_gap;
box_h     = z_glass_b + glass_t;          // 口沿与玻璃面齐平

hx0 = (scr_pcb_l - hole_dx) / 2;          // 5.25
hy0 = (scr_pcb_w - hole_dy) / 2;          // 5.0
boss_xy = [[hx0, hy0], [hx0 + hole_dx, hy0],
           [hx0, hy0 + hole_dy], [hx0 + hole_dx, hy0 + hole_dy]];

echo(str("盒体外形: ", box_l, " x ", box_w, " x ", box_h, " mm"));
echo(str("扩展板底面 z=", z_exp_bot, "  屏板底面 z=", z_scr_bot,
         "  玻璃底 z=", z_glass_b));
echo(str("螺丝参考长度 ≈ ",
         floor_t - cb_depth + bat_h + exp_t + (stack_h - pillar_len) + 3,
         " mm（含悬空 ", stack_h - pillar_len, " + 旋入铜柱 3）"));
assert(z_glass_b > z_scr_bot, "高度链错误");

/* ---------- 基础形 ---------- */
module rrect(l, w, r) { offset(r = r) offset(r = -r) square([l, w]); }

module box_shell() {
    difference() {
        translate([box_x0, box_y0, 0])
            linear_extrude(box_h) rrect(box_l, box_w, corner_r);
        translate([cav_x0, cav_y0, floor_t])
            linear_extrude(box_h - floor_t + 0.01)
                rrect(cav_x1 - cav_x0, cav_y1 - cav_y0, glass_r + fit_glass);
    }
}

/* ---------- 楔形凸台基元（底部 45° 自支撑，免打印支撑）---------- */
// 沿 X 挤出，剖面在 YZ；y_in=内侧面，y_wall=贴墙面
module wedge_yz(x0, w, y_in, y_wall, z_top, z_bot) {
    d = abs(y_wall - y_in);
    translate([x0, 0, 0]) rotate([90, 0, 90])
        linear_extrude(w)
            polygon([[y_in, z_bot], [y_in, z_top],
                     [y_wall, z_top], [y_wall, z_bot - d]]);
}
// 沿 Y 挤出，剖面在 XZ
module wedge_xz(y0, w, x_in, x_wall, z_top, z_bot) {
    d = abs(x_wall - x_in);
    translate([0, y0 + w, 0]) rotate([90, 0, 0])
        linear_extrude(w)
            polygon([[x_in, z_bot], [x_in, z_top],
                     [x_wall, z_top], [x_wall, z_bot - d]]);
}

/* ---------- 玻璃支撑舌（顶2 / 底3 / USB侧2 / C6侧2，共 9 处）---------- */
module glass_tabs() {
    zt = z_glass_b;  zb = z_glass_b - tab_t;
    for (xc = [12, 75])                    // 顶(Y+)：左移靠边，避开 POWER 沉窝
        wedge_yz(xc - tab_w / 2, tab_w, cav_y1 - tab_p, cav_y1 + 0.01, zt, zb);
    for (xc = [15, scr_pcb_l / 2, 87])     // 底(Y-)：整面无开孔，均布 3 个
        wedge_yz(xc - tab_w / 2, tab_w, cav_y0 + tab_p, cav_y0 - 0.01, zt, zb);
    for (yc = [12, 50])                    // 右/USB(X+)：避开 12×8 大孔(y32.6-44.6)
        wedge_xz(yc - tab_w / 2, tab_w, cav_x1 - tab_p, cav_x1 + 0.01, zt, zb);
    for (yc = [15, 45])                    // 左/C6(X-)：加深 5
        wedge_xz(yc - tab_w / 2, tab_w, cav_x0 + tab_p_left, cav_x0 - 0.01, zt, zb);
}

/* ---------- 套筒柱（φ2.9 通孔，无螺母）---------- */
module bosses() {
    for (p = boss_xy)
        translate([p[0], p[1], floor_t])
            cylinder(d = boss_d, h = bat_h);
}
module boss_bores() {  // 通孔 + 底面沉孔，最后统一减去
    for (p = boss_xy) translate([p[0], p[1], 0]) {
        translate([0, 0, -0.01]) cylinder(d = screw_d, h = floor_t + bat_h + 0.02);
        translate([0, 0, -0.01]) cylinder(d = cb_d, h = cb_depth + 0.01);
    }
}

/* ---------- 穿墙槽 / 沉槽切割体 ---------- */
module slot_cut_x(y, z, w, h, from_x, len) {  // 沿 +X 切
    translate([from_x, y, z]) rotate([0, 90, 0]) rotate([0, 0, 90])
        linear_extrude(len)
            translate([-w / 2, -h / 2]) rrect(w, h, min(h, w) / 2 - 0.01);
}
module slot_cut_y(x, z, w, h, from_y, len) {  // 沿 +Y 切
    translate([x, from_y, z]) rotate([-90, 0, 0])
        linear_extrude(len)
            translate([-w / 2, -h / 2]) rrect(w, h, min(h, w) / 2 - 0.01);
}

cut_len = wall_t + max(glass_ov_xm, glass_ov_xp, glass_ov_y) + 4;

module wall_cutouts() {
    /* --- H1 Type-C（X+）：穿墙槽 + 外侧浅沉槽 --- */
    z_usb = z_scr_bot + usb_zoff;
    slot_cut_x(h1_y, z_usb, usb_w, usb_h, cav_x1 - 1, cut_len);

    /* --- POWER 按键（Y+）：导孔 + 内壁沉窝（卡按压销法兰）--- */
    z_btn = z_scr_bot + btn_zoff;
    for (bx = btns) {
        slot_cut_y(bx, z_btn, btn_d, btn_d, cav_y1 - 1, cut_len);
        translate([bx, cav_y1 - btn_pkt_dp, z_btn])
            rotate([-90, 0, 0]) cylinder(d = btn_pkt_d, h = btn_pkt_dp + 1);
    }

    /* --- SD 卡槽（Y+）：穿墙槽 + 外侧圆角浅沉槽 + 圆角指甲槽 --- */
    z_sd = z_scr_bot + sd_zoff;
    slot_cut_y(sd_cx, z_sd, sd_w, sd_h, cav_y1 - 1, cut_len);
    slot_cut_y(sd_cx, z_sd, sd_rec[0], sd_rec[1],
               box_y1 - rec_depth, rec_depth + 1);
    // 指甲槽：竖向圆角槽，贯穿，与卡槽成十字
    slot_cut_y(sd_cx, z_sd, sd_notch, sd_rec[1] + 5, cav_y1 - 1, cut_len);

    /* --- SMA ×2（X- 壁）：穿孔 + 内侧沉窝 --- */
    for (sy = sma_y) {
        translate([box_x0 - 1, sy, sma_z])
            rotate([0, 90, 0]) cylinder(d = sma_d, h = wall_t + 2);
        translate([cav_x0 - sma_cb_dp, sy, sma_z])
            rotate([0, 90, 0]) cylinder(d = sma_cb_d, h = sma_cb_dp + 1);
    }
}

/* ---------- 底面：条栅镂空（减去）与外凸脚（叠加）---------- */
module floor_slats() {
    if (lighten)
        for (x = slat_xs)
            translate([x - slat_w / 2, slat_y0, -0.01])
                cube([slat_w, slat_y1 - slat_y0, floor_t + 0.02]);
}
module feet() {
    for (p = [[box_x0 + foot_inset, box_y0 + foot_inset],
              [box_x1 - foot_inset, box_y0 + foot_inset],
              [box_x0 + foot_inset, box_y1 - foot_inset],
              [box_x1 - foot_inset, box_y1 - foot_inset]])
        translate([p[0], p[1], -foot_h])
            cylinder(d1 = foot_d, d2 = foot_d + 2, h = foot_h + 0.01);
}

/* ---------- 按键按压销（法兰卡进内壁沉窝）---------- */
module plunger() {  // 自下而上：顶鼻(按压面) → 法兰 → 穿墙杆
    shaft_len = (wall_t - btn_pkt_dp) + pl_proud;
    cylinder(d = pl_shaft_d, h = pl_nose + pl_flange_t + shaft_len);
    translate([0, 0, pl_nose]) cylinder(d = pl_flange_d, h = pl_flange_t);
}

/* ---------- 总装 ---------- */
module main_box() {
    difference() {
        union() {
            box_shell();
            glass_tabs();
            bosses();
            feet();
        }
        boss_bores();
        wall_cutouts();
        floor_slats();
    }
}

/* ---------- 一体打印盘：按键销立在底边(Y-)栅格前的空地 ----------
   法兰朝下悬 0.4，单根锥形断颈（底 φ2.2 / 顶 φ0.8）接法兰底面
   正中心：断裂点被引导到细端，残根柱留在盒底板上（对内不可见），
   销子上仅剩 φ0.8 小点，免打磨。
   断口不能放细杆侧面——按压时整根杆会滑过 φ4.1 导孔，有残留必卡。
   放底边栅格前的实心地板带（栅格 y≥10），四周无螺柱方便下手拧断；
   销子在装电池前取下，占电池投影区无妨。 */
module print_plate() {
    main_box();
    for (i = [0:n_plungers - 1]) {
        px = 30 + i * 14;  py = 5.5;
        translate([px, py, floor_t + 0.4]) plunger();
        translate([px, py, floor_t - 0.01])
            cylinder(d1 = 2.2, d2 = 0.8, h = 0.5 + 0.01);
    }
}

ozero = align_origin ? [-box_x0, -box_y0, foot_h] : [0, 0, 0];
echo(str("align_origin=", align_origin,
         "  屏板原点@", [-box_x0, -box_y0, foot_h + floor_t] , " (盒内底面高)"));

translate(ozero) {
    if (part == "print") print_plate();
    if (part == "box") main_box();
    if (part == "section")
        difference() {
            main_box();
            translate([scr_pcb_l / 2, box_y0 - 1, -1])
                cube([box_l, box_w + 2, box_h + 2]);
        }
}
if (part == "plungers")
    for (i = [0:len(btns) - 1]) translate([i * 12, 0, 0]) plunger();
