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
fit_glass = 0.10;     // 玻璃四周单边间隙。9600 树脂实测：模型 115.2x67.68
                      // 印出 115.4~115.6 x 67.6~67.9，即 X 向偏大 0.2~0.4、
                      // Y 向 -0.08~+0.22。取 0.1 是让最坏一档(Y 偏小 0.08)
                      // 仍有 0.12 总间隙；取 0 会装不进去。
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
glass_bot  = -glass_ov_y;      // 玻璃下(40PIN)边缘 y

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
slot_r     = 2.0;     // 开孔圆角上限（原自动取 min(w,h)/2，孔成胶囊形）

/* ---------- 开孔标识凸字 ---------- */
emb_h    = 0.4;       // 凸起高度
emb_sink = 0.1;       // 字根沉入墙面量（保证与墙体实体相交）
emb_sz   = 1.8;       // 字号（大写字高约 1.3）
emb_font = "Liberation Sans:style=Bold";
lbl_z    = 13.0;      // 统一基线高度（字缩小后上移贴近开孔；下缘留 0.6+）
lbl_pwr  = "PWR";
lbl_sd   = "SD";
lbl_usb  = "USB";     // H1 = USB TO UART；想标 UART 改这里
lbl_sma  = ["GPS", "ADS-B"];   // 对应 sma_y 的两个孔，顺序可对调

/* ---------- 玻璃支撑舌 ---------- */
tab_w = 8;            // 舌宽
tab_t = 1.5;          // 舌厚（垂直方向）
tab_p = 2.8;          // 自内壁面伸出量
tab_p_left = 5.0;     // 左(C6)侧加深：该侧玻璃悬出最大，多托一点

/* ---------- 电池 ----------
   955465 软包（65×54×9.5）自由平放，上被扩展板压住、周围是柱子，
   不做定位挡墙。注意躲开扩展板底面 x≥76 的 SDR 悬垂区。 */

/* ---------- USB H1（X+ 壁）---------- */
h1_y     = glass_bot + 41.25;  // 量尺 y=43.20（距玻璃下边 41.25）
usb_w    = 11.5;  usb_h = 7.0; // 直插穿墙孔：公头包胶(宽11)整体穿入，
                               // 实测插满时包胶面与玻璃边平齐，故不做外沉槽
usb_glass_d = 9.35;            // 量尺 z=19.80
usb_rec  = [14.5, 10];         // 外侧圆角浅沉槽：四周统一 1.5 边框
usb_zoff = glass_t + scr_glass_gap + scr_pcb_t - usb_glass_d;  // -2.35
rec_depth = 0.5;               // SD 外沉深度（1.8 壁余厚 1.3）

/* ---------- 按键（Y+ 壁）：只开 POWER ---------- */
// 实测：中心=玻璃左+31(POWER) / +39(BOOT) / +47(RESET)
btn_power_x = glass_left + 31;     // 22.8，离 SD 最远
btns        = [btn_power_x];       // 需要 RESET 时加 glass_left+47
btn_d       = 4.1;                 // 导孔（配打印按压销）
btn_glass_d = 8.85;                // 量尺 z=20.30
btn_rec_d   = 8.0;                 // 外侧圆形浅凹（深 rec_depth）
btn_zoff    = glass_t + scr_glass_gap + scr_pcb_t - btn_glass_d;
btn_key_depth = 3.2;               // 键帽面到玻璃边缘。两次实测标定：
                                   // ① 旧件销尾静止凸 1.95/触发 1.51 → 行程 0.44
                                   // ② 旧件销总长 7.80 预压按键，削到 7.65 正常
                                   // 回代旧几何（法兰止位 64.84）得键帽 y≈60.25，
                                   // 即距玻璃边 3.2。原假设 4.0 偏深 0.8。
// 防倾斜：原来导向孔只剩 0.8mm（壁 1.8 减去 1.0 深沉窝），销子能歪 26°。
// 改为内壁加一段导向套，导向长度 = wall_t + btn_bore_in，法兰退到套内沉腔止位。
btn_boss_d  = 9.0;                 // 导向套外径（沉腔 6.6 外留 1.2 壁厚）
btn_guide   = 2.3;                 // 导向套自内壁面伸入长度（末端离键帽 1.1）
btn_bore_in = 0.95;                // 其中做导向孔的前段（其余为法兰沉腔）
btn_pkt_d   = 6.6;                 // 法兰沉腔直径（法兰 6.2，留 0.4 活动量）
pl_shaft_d  = 3.7;                 // 按压销杆径
pl_flange_d = 6.2;  pl_flange_t = 1.0;
pl_nose     = 1.0;                 // 顶鼻：够长才不会按到底时法兰撞屏板边缘
pl_proud    = 1.2;                 // 静止时销尾高出外壁量（原 2.0 太凸）
n_plungers  = 4;                   // 一体打印的按键销数量（1 用 2 备）

/* ---------- SD 卡槽（Y+ 壁）---------- */
sd_cx    = glass_left + 59;    // 50.8。实测：卡座中心=玻璃左+59
sd_w     = 16.0;  sd_h = 3.5;
sd_glass_d = 9.35;             // 量尺 z=19.80
sd_zoff  = glass_t + scr_glass_gap + scr_pcb_t - sd_glass_d;      // -1.85
sd_rec   = [22, 8];            // 外侧圆角沉槽（深 rec_depth）
sd_notch = 10;                 // 指甲槽宽（圆角，贯穿余壁）

/* ---------- SMA ×2（X- 壁，面板式穿墙）---------- */
sma_y = [19, 41];     // 两孔中心 y
sma_z = 19;           // 孔中心高（自内部坐标系底面；盒子变矮后下调留口沿余量）
sma_d = 6.4;          // 面板 SMA 螺纹穿孔
sma_cb_d = 9.5;  sma_cb_dp = 0.6;   // 内侧沉窝：留平面好上螺母
sma_rec_d = 9.45; sma_rec_dp = 0.5;  // 外侧圆凹：卡垫片（深度按垫片厚度调）

/* ---------- 1/4"-20 云台转接板（外挂盒底，与 4 颗底部螺丝共用）----------
   载荷路径：云台螺柱自板底旋入 → 螺母坐在板内六角腔 → 腔底实体承拉。
   螺母腔开口朝上，装到盒底后被盒体外底面封住，螺母跑不掉。
   打印时用断颈筋悬在盒内，一个文件一个价；装配前掰下来。 */
mount_plate = true;
mp_t        = 6.0;    // 板厚
mp_nut_af   = 11.3;   // 1/4-20 螺母对边（标准 11.11 + 0.2 装配间隙）
mp_nut_t    = 3.4;    // 螺母腔深：配 3.2 薄螺母。换 5.56 标准螺母则改 5.8，
                      // 同时 mp_t 加到 8.5、螺丝加长 2.5
mp_stud_d   = 7.0;    // 云台螺柱过孔（φ6.35 + 间隙）
mp_arm_w    = 12;     // 臂宽
mp_pad_d    = 12;     // 螺丝端圆盘直径
mp_hub_d    = 24;     // 中央凸台直径
mp_cb_d     = 5.6;  mp_cb_dp = 1.4;   // 板底沉孔（容螺丝头）
mp_z        = 16;     // 打印时悬在盒内的高度
mp_tab      = 1.6;    // 断颈连接筋截面（斜口钳剪断）

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
mp_cx = (box_x0 + box_x1) / 2;
mp_cy = (box_y0 + box_y1) / 2;
assert(mp_t - mp_nut_t >= 2.0, "转接板螺母腔下方肉太薄：加大 mp_t 或减小 mp_nut_t");
echo(str("云台板: ", mp_t, " 厚  1/4-20 螺母腔 对边", mp_nut_af, " 深", mp_nut_t,
         "  腔底承拉肉 ", mp_t - mp_nut_t,
         "  装板后总高 ", box_h + mp_t,
         "  螺丝需 ≈ ", mp_t - mp_cb_dp + floor_t + bat_h + exp_t
                        + (stack_h - pillar_len) + 3));
echo(str("螺丝参考长度 ≈ ",
         floor_t - cb_depth + bat_h + exp_t + (stack_h - pillar_len) + 3,
         " mm（含悬空 ", stack_h - pillar_len, " + 旋入铜柱 3）"));
assert(z_glass_b > z_scr_bot, "高度链错误");

// 按键几何自检（y 向，越小越靠近屏板中心）
// 关键：屏组件由玻璃在口内定位，而玻璃有 ±fit_glass 的游动量，所以键帽的
// y 位置不是一个点而是一段区间。旧件"松着能按、拧紧就顶住"就是这么来的
// （旧 fit_glass=0.4 → 游动 0.8，足够把键帽推到销子上）。两端都要校验。
btn_key_y  = gy1 - btn_key_depth;                             // 键帽面（玻璃居中）
btn_key_wc = gy1 + fit_glass - btn_key_depth;                 // 玻璃靠按键壁：最易预压
btn_key_bc = gy1 - fit_glass - btn_key_depth;                 // 玻璃靠对侧：最费行程
btn_tip_y  = cav_y1 - btn_bore_in - pl_flange_t - pl_nose;    // 顶鼻静止位
btn_gap    = btn_tip_y - btn_key_y;
btn_gap_wc = btn_tip_y - btn_key_wc;
btn_travel = (btn_tip_y - btn_key_bc) + 0.25;
echo(str("按键: 导向长 ", wall_t + btn_bore_in, "  静止间隙 ", btn_gap,
         "（最坏 ", btn_gap_wc, "）  需行程 ", btn_travel, " / 可用 ", pl_proud,
         "  销总长 ", pl_nose + pl_flange_t + wall_t + btn_bore_in + pl_proud));
assert(btn_gap_wc > 0.1, "玻璃偏向按键壁时销子会预压按键：减小 pl_nose");
assert(btn_travel < pl_proud, "行程不够：加大 pl_proud");
assert(cav_y1 - btn_bore_in - pl_flange_t - btn_travel > scr_pcb_w + 0.3,
       "按到底时法兰会撞屏板边缘：加大 pl_nose");
assert(cav_y1 - btn_guide > btn_key_y + 1.0, "导向套会顶到键帽：缩短 btn_guide");

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
            translate([-w / 2, -h / 2]) rrect(w, h, min(slot_r, min(h, w) / 2 - 0.01));
}
module slot_cut_y(x, z, w, h, from_y, len) {  // 沿 +Y 切
    translate([x, from_y, z]) rotate([-90, 0, 0])
        linear_extrude(len)
            translate([-w / 2, -h / 2]) rrect(w, h, min(slot_r, min(h, w) / 2 - 0.01));
}

cut_len = wall_t + max(glass_ov_xm, glass_ov_xp, glass_ov_y) + 4;

module wall_cutouts() {
    /* --- H1 Type-C（X+）：穿墙槽 + 外侧浅沉槽 --- */
    z_usb = z_scr_bot + usb_zoff;
    slot_cut_x(h1_y, z_usb, usb_w, usb_h, cav_x1 - 1, cut_len);
    slot_cut_x(h1_y, z_usb, usb_rec[0], usb_rec[1],
               box_x1 - rec_depth, rec_depth + 1);

    /* --- POWER 按键（Y+）：长导向孔 + 套内法兰沉腔 --- */
    z_btn = z_scr_bot + btn_zoff;
    for (bx = btns) {
        slot_cut_y(bx, z_btn, btn_d, btn_d, cav_y1 - btn_bore_in, cut_len);
        translate([bx, cav_y1 - btn_guide - 0.5, z_btn]) rotate([-90, 0, 0])
            cylinder(d = btn_pkt_d, h = btn_guide - btn_bore_in + 0.5);
        translate([bx, box_y1 - rec_depth, z_btn]) rotate([-90, 0, 0])
            cylinder(d = btn_rec_d, h = rec_depth + 0.01);
    }

    /* --- SD 卡槽（Y+）：穿墙槽 + 外侧圆角浅沉槽 + 圆角指甲槽 --- */
    z_sd = z_scr_bot + sd_zoff;
    slot_cut_y(sd_cx, z_sd, sd_w, sd_h, cav_y1 - 1, cut_len);
    slot_cut_y(sd_cx, z_sd, sd_rec[0], sd_rec[1],
               box_y1 - rec_depth, rec_depth + 1);

    /* --- SMA ×2（X- 壁）：穿孔 + 内侧沉窝 --- */
    for (sy = sma_y) {
        translate([box_x0 - 1, sy, sma_z])
            rotate([0, 90, 0]) cylinder(d = sma_d, h = wall_t + 2);
        translate([cav_x0 - sma_cb_dp, sy, sma_z])
            rotate([0, 90, 0]) cylinder(d = sma_cb_d, h = sma_cb_dp + 1);
        translate([box_x0 - 0.01, sy, sma_z])
            rotate([0, 90, 0]) cylinder(d = sma_rec_d, h = sma_rec_dp + 0.01);
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

/* ---------- 开孔标识凸字（外壁，0.4 凸起）----------
   face: 0=Y+壁(PWR/SD)  1=X+壁(USB)  2=X-壁(SMA)。
   三种朝向的旋转分别让字面法线指向 +y / +x / -x，从外面看正读。 */
module wall_text(txt, u, z, face) {
    // 起点沉入墙面 emb_sink：与墙必须有实体重叠，仅共面会被算成独立体
    if (face == 0)
        translate([u, box_y1 - emb_sink, z]) rotate([-90, 0, 0]) rotate([0, 0, 180])
            linear_extrude(emb_h + emb_sink)
                text(txt, size = emb_sz, font = emb_font,
                     halign = "center", valign = "center");
    else if (face == 1)
        translate([box_x1 - emb_sink, u, z]) rotate([90, 0, 90])
            linear_extrude(emb_h + emb_sink)
                text(txt, size = emb_sz, font = emb_font,
                     halign = "center", valign = "center");
    else
        translate([box_x0 + emb_sink, u, z]) rotate([90, 0, -90])
            linear_extrude(emb_h + emb_sink)
                text(txt, size = emb_sz, font = emb_font,
                     halign = "center", valign = "center");
}
module labels() {
    wall_text(lbl_pwr, btn_power_x, lbl_z, 0);
    wall_text(lbl_sd,  sd_cx,       lbl_z, 0);
    wall_text(lbl_usb, h1_y,        lbl_z, 1);
    wall_text(lbl_sma[0], sma_y[0], lbl_z, 2);
    wall_text(lbl_sma[1], sma_y[1], lbl_z, 2);
}

/* ---------- 按键导向套（内壁，加长导向防倾斜）---------- */
module btn_guides() {
    z_btn = z_scr_bot + btn_zoff;
    for (bx = btns)
        translate([bx, cav_y1 - btn_guide, z_btn]) rotate([-90, 0, 0])
            cylinder(d = btn_boss_d, h = btn_guide + 0.01);
}

/* ---------- 按键按压销（法兰止位于导向套内沉腔）---------- */
module plunger() {  // 自下而上：顶鼻(按压面) → 法兰 → 导向杆
    out_len = wall_t + btn_bore_in + pl_proud;
    cylinder(d = pl_shaft_d, h = pl_nose + pl_flange_t + out_len);
    translate([0, 0, pl_nose]) cylinder(d = pl_flange_d, h = pl_flange_t);
}

/* ---------- 总装 ---------- */
module main_box() {
    difference() {
        union() {
            box_shell();
            glass_tabs();
            bosses();
            btn_guides();
            labels();
            feet();
        }
        boss_bores();
        wall_cutouts();
        floor_slats();
    }
}

/* ---------- 1/4"-20 云台转接板 ---------- */
module mp_profile() {                 // 2D：中央凸台 + 4 条锥形臂到螺丝盘
    union() {
        translate([mp_cx, mp_cy]) circle(d = mp_hub_d);
        for (q = boss_xy)
            hull() {
                translate(q) circle(d = mp_pad_d);
                translate([mp_cx, mp_cy]) circle(d = mp_arm_w);
            }
    }
}
module mount_plate_body() {
    difference() {
        linear_extrude(mp_t) mp_profile();
        for (q = boss_xy) translate([q[0], q[1], -0.01]) {
            cylinder(d = screw_d, h = mp_t + 0.02);          // 螺丝过孔
            cylinder(d = mp_cb_d, h = mp_cb_dp + 0.01);      // 板底沉孔
        }
        translate([mp_cx, mp_cy, -0.01])                     // 云台螺柱过孔
            cylinder(d = mp_stud_d, h = mp_t + 0.02);
        translate([mp_cx, mp_cy, mp_t - mp_nut_t])           // 六角螺母腔（朝上）
            cylinder(d = mp_nut_af / cos(30), h = mp_nut_t + 0.01, $fn = 6);
    }
}
module mp_tabs() {                    // 4 根断颈筋：焊盘外缘 → 最近内壁
    for (q = boss_xy) {
        low = q[1] < scr_pcb_w / 2;   // 两端各多咬 0.6/1.0，避免只相切
        ya  = low ? cav_y0 - 0.6 : q[1] + mp_pad_d / 2 - 1.0;
        yb  = low ? q[1] - mp_pad_d / 2 + 1.0 : cav_y1 + 0.6;
        translate([q[0] - mp_tab / 2, ya, mp_z + mp_t / 2 - mp_tab / 2])
            cube([mp_tab, yb - ya, mp_tab]);
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
    if (mount_plate) {
        translate([0, 0, mp_z]) mount_plate_body();
        mp_tabs();
    }
}

ozero = align_origin ? [-box_x0, -box_y0, foot_h] : [0, 0, 0];
/* ---------- 数字尺子：全部特征的绝对坐标 ----------
   基准角 = 盒体【外底壁面】× C6 侧外壁 × 40PIN 侧外壁。
   即：盒子正放，从左下外角量起；z 不含凸脚（凸脚只在四角，另凸 foot_h）。 */
function AX(v) = v - box_x0;   // x: 0 = C6 侧（面朝屏幕的左）外壁
function AY(v) = v - box_y0;   // y: 0 = 40PIN 侧（屏幕下）外壁
function AZ(v) = v;            // z: 0 = 盒底外壁面
echo("======== 量尺表（基准=盒底外壁面 × 左下外角，mm）========");
echo(str("外形 ", box_l, " x ", box_w, " x ", box_h, "  凸脚另凸 ", foot_h, "（仅四角）"));
echo(str("玻璃边: 左x=", AX(gx0), " 右x=", AX(gx1),
         " 下y=", AY(gy0), " 上y=", AY(gy1), " 面z=", AZ(box_h)));
echo(str("层高 z: 盒外底0 / 盒内底", AZ(floor_t), " / 扩展板底", AZ(z_exp_bot),
         " / 屏板底", AZ(z_scr_bot), " / 玻璃底", AZ(z_glass_b),
         " / 口沿=玻璃面", AZ(box_h)));
echo(str("POWER 孔  x=", AX(btn_power_x), "  z=", AZ(z_scr_bot + btn_zoff),
         " (距玻璃面 ", btn_glass_d, ")  φ", btn_d,
         "  外凹φ", btn_rec_d, "深", rec_depth, "  在 y=", AY(box_y1), " 壁"));
echo(str("SD 槽     x=", AX(sd_cx), "  z=", AZ(z_scr_bot + sd_zoff),
         " (距玻璃面 ", sd_glass_d, ")  ", sd_w, "x", sd_h,
         "  外框", sd_rec[0], "x", sd_rec[1], " 深", rec_depth));
echo(str("USB 孔    y=", AY(h1_y), "  z=", AZ(z_scr_bot + usb_zoff),
         " (距玻璃面 ", usb_glass_d, ")  ", usb_w, "(y向)x", usb_h,
         "(z向)  外框", usb_rec[0], "x", usb_rec[1], "深", rec_depth,
         "  在 x=", AX(box_x1), " 壁"));
echo(str("SMA 孔    y=", AY(sma_y[0]), " 和 ", AY(sma_y[1]),
         "  z=", AZ(sma_z), " (距玻璃面 ", box_h - sma_z, ")  φ", sma_d,
         "  外凹φ", sma_rec_d, "深", sma_rec_dp, "  在 x=", AX(box_x0), " 壁"));
echo(str("螺柱中心  x=", AX(boss_xy[0][0]), "/", AX(boss_xy[1][0]),
         "  y=", AY(boss_xy[0][1]), "/", AY(boss_xy[2][1]),
         "  柱顶z=", AZ(floor_t + bat_h), "  孔φ", screw_d));
echo(str("玻璃口    ", cav_x1 - cav_x0, " x ", cav_y1 - cav_y0,
         "  单边间隙", fit_glass, "  台阶面z=", AZ(z_glass_b)));
echo("===================================================");
echo(str("align_origin=", align_origin,
         "  屏板原点@", [-box_x0, -box_y0, foot_h + floor_t] , " (盒内底面高)"));

translate(ozero) {
    if (part == "print") print_plate();
if (part == "plate") translate([0, 0, mp_z]) mount_plate_body();
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
