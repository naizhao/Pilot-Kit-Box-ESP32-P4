// =============================================================
// Pilot Kit Box — 4.3" 触摸屏版 3D 打印底座（盒体）v4.11 / FDM
// =============================================================
// V4：统一圆角主壳 + 后坡可拆SMA盖 + 可换GPS背托；原SCAD独立保留。
// 字框定位使用textmetrics()；本版在OpenSCAD 2026.10.13验证，需支持该函数的版本。
//
// 紧固链（自下而上，底座不埋螺母）：
//   M2.5 螺丝从底面沉孔穿入 → 打印套筒柱（φ2.9 通孔）
//   → 扩展板安装孔 → 跨过 HAT 对接造成的悬空段（stack_h−铜柱长）
//   → 锁进挂在微雪板下的 3.8 铜柱。
//   板间距 7.0 是历史初值，不能把两个塑胶座高度相加当作插合后净距；
//   铜柱 3.8，隔套须按实际插合净距配合，不能用紧固力强行拉合 HAT。
//   玻璃支撑面固定；exp_support_adjust 只调整扩展板支撑及其接口高度。
//
// 减料设计：
//   - 内腔按玻璃轮廓，板/屏高度链保留独立校准；
//     SD 卡座 / SMA 线柱等板边突出物不会顶墙
//   - 玻璃仅靠周圈薄支撑舌托住（螺丝已锁死整叠，只需扣住）
//   - 底板条栅镂空，侧壁 1.8；SMA承压面完整，盖拆下后安装螺母。
//
// 打印提示：默认将主盒、盖、陶瓷托盘和4根按键销平放在同一180mm平台。
//   不生成内置打印支撑；SD门面保留在顶盖上，供无支撑试打。
//   云台转接板仍用 part="plate" 单独导出；托盘上槽后挡等其余悬空请切片检查。
//   沉孔配盘头/沉头 M2.5（圆柱头太高会露出底面）。
//
// 坐标系（俯视，屏朝上，原点 = 屏板 X-/Y- 角）：
//   X+ = USB 壁（H1 Type-C，玻璃悬出 3.7）
//   X- = C6 壁（玻璃悬出 8.2）；SMA 已移到 Y+ 顶部接口舱
//   Y+ = 朝天顶部：SD 卡槽；背板侧加深接口舱容纳 SMA / 内置 GPS
//   Y- = 40PIN 边（不开孔）
//
// 尺寸来源：
//   - 微雪 dimensions.pdf（20260411）：玻璃 114.4×66.8，
//     PCB 102.5×60，孔距 92×50 M2.5，厚 11.15（玻璃 1.6/板下净空 4）
//   - 扩展板 v4.11 正式 PCB：100×62，原点为 KiCad (50,50)，
//     4 孔 drill φ2.7 @ (4,6)/(96,6)/(4,56)/(96,56)，孔距同 92×50
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
//   2. stack_h —— 两板完全插合后的净距；不能由铜柱长度单独确定
//   3. J4 / LED 中心高度、SMA 螺母和尾柱包络 —— 装机前按实物校准
// =============================================================

/* ---------- 输出选择 ---------- */
part = "layout";               // 默认同板排齐全部打印件；box / print 保留仅主盒的旧接口
                      // cap / gps_tray / plungers / plate 独立平放打印；exploded / gps_section 预览
                      // "assembly" 显示塑料件装配；检查包络需另开 preview_hardware
align_origin = true;  // true = 输出平移到包围盒角点(0,0,0)，方便量尺；
                      // 内部坐标系不变（原点=屏板 X-/Y- 角）

/* ---------- 打印公差 ---------- */
fit_glass = 0.25;     // FDM 玻璃四周单边间隙；首件按实际打印尺寸校准
$fn = 64;


/* ---------- 屏组件（微雪板）---------- */
glass_l = 114.4;  glass_w = 66.88; glass_t = 1.0;  glass_r = 2.5;  // 玻璃实测 114.4×66.88，厚+背胶 1.0
scr_pcb_l = 102.5;  scr_pcb_w = 60.0;  scr_pcb_t = 1.6;
scr_under = 4.0;                       // 图纸背部尺寸参考，不作为HAT插合高度
glass_to_screen_back = 6.15;           // 玻璃内支撑面→屏PCB背面；沿用原模型初值，待实物校准
scr_glass_gap = glass_to_screen_back - scr_pcb_t;
glass_ov_xm = 8.2;    // 玻璃悬出：X-（非 USB 侧）
glass_ov_xp = 3.7;    // 玻璃悬出：X+（USB 侧）
glass_ov_y  = (glass_w - scr_pcb_w) / 2;                  // 3.4
// 方位约定（面朝屏幕，PCB 在后）：左=C6 侧(X-)，右=USB H1/H2 侧(X+)，
// 顶=SD/按键边(Y+)。开孔位一律以玻璃边缘实测值标注。
glass_left = -glass_ov_xm;     // 玻璃左边缘 x（内部坐标）
glass_bot  = -glass_ov_y;      // 玻璃下(40PIN)边缘 y

/* ---------- 扩展板 ---------- */
exp_t = 1.6;
exp_l = 100;  exp_w = 62;              // 板框（Y 向比屏板各多 1mm，见 §2a）
hole_dx = 92;  hole_dy = 50;           // 安装孔距（两板一致）
// PCB 坐标 → 本文件坐标的换算。两板孔距相同，所以 X 只差板宽之半：
exp_x_off = (scr_pcb_l - exp_l) / 2;   // = 1.25
// 输入为相对板框 (50,50) 的 PCB 坐标；用安装孔对齐，板框 Y=-1…61。
function exp_y(y_pcb) = (scr_pcb_w + exp_w) / 2 - y_pcb;
function exp_x(x_pcb) = x_pcb + exp_x_off;

/* ---------- 高度链（固定玻璃支撑面向下）---------- */
floor_t = 2.4;        // 底板厚（含沉孔）
glass_seat_z = 28.15; // 玻璃内支撑面到盒底外面的距离；保持原外形
stack_h    = 7.0;     // HAT插合板间净距的历史初值，非已核实连接器规格
exp_support_adjust = 0; // 正值抬高扩展板，使其靠近屏板；不是按目测缝隙直接补高
pillar_len = 3.8;     // 屏板下铜柱长；隔套参考值由实际 board_gap 推导

/* ---------- 壁与柱 ---------- */
wall_t     = 1.8;
boss_d     = 7.0;
screw_d    = 2.9;     // M2.5 通孔
cb_d       = 5.6;     // 底面沉孔（圆柱头/盘头）
cb_depth   = 1.4;
corner_r   = 3.0;
slot_r     = 2.0;     // 开孔圆角上限（原自动取 min(w,h)/2，孔成胶囊形）

lbl_pwr  = "PWR";
lbl_sd   = "SD";
lbl_usb  = "USB";     // H1 = USB TO UART；想标 UART 改这里
lbl_sma  = ["ADS-B", "AIR", "GPS"]; // 全部内GPS配置均保留三个孔

/* ---------- 玻璃支撑舌 ---------- */
tab_w = 8;            // 舌宽
tab_t = 1.5;          // 舌厚（垂直方向）
tab_p = 2.8;          // 自内壁面伸出量
tab_p_left = 5.0;     // 左(C6)侧加深：该侧玻璃悬出最大，多托一点

/* ---------- 电池 ----------
   955465 软包（65×54×9.5）自由平放，上被扩展板压住、周围是柱子，
   不做定位挡墙。V4.11已集成接收链，无旧版底面SDR模块；加喇叭后电池靠左放。 */

/* ---------- USB后方底板喇叭浅仓（用户给定完整外廓，非厂家安装图） ---------- */
speaker_enabled = true;
speaker_size = [26,26,4.3];
speaker_xy = [90,34];         // X+/USB侧底部，两根右侧螺柱之间
speaker_fit = .3;            // 浅仓单边装配余量；26方框可直接放入
speaker_recess = 1.2;        // 从2.4底板内面下沉，周边余底1.2
speaker_tape_t = .3;         // 外框胶层/垫片预留；不把胶贴到振膜
speaker_diaphragm = [11,17]; // 用户确认：振膜宽X=11/高Y=17，圆角，朝盒底
speaker_hole_d = 2;
speaker_hole_pitch = 3.6;
speaker_wire_w = 2.4;        // 向X+墙内空带的顶部开放线槽，不穿侧墙
speaker_wire_offset = 0;     // 沿Y调整以匹配焊片，线从上方放入
speaker_z = floor_t-speaker_recess+speaker_tape_t;

/* ---------- USB H1（X+ 壁）---------- */
h1_y     = glass_bot + 41.25;  // 量尺 y=43.20（距玻璃下边 41.25）
usb_w    = 11.5;  usb_h = 7.0; // 直插穿墙孔：公头包胶(宽11)整体穿入，
                               // 实测插满时包胶面与玻璃边平齐，浅槽不能替代穿孔
usb_glass_d = 9.35;            // 量尺 z=19.80
usb_rec  = [14.5, 10];         // H1 浅槽外廓；J4 沿用相同扩边量，连成整体并避让 LED
usb_zoff = glass_t + scr_glass_gap + scr_pcb_t - usb_glass_d;  // -2.35
rec_depth = 0.5;               // 接口浅槽深度（1.8 壁余厚 1.3）

/* ---------- v4.11 USB J4 与 D8/D9 侧面观察窗（X+ 壁）---------- */
j4_pcb = [95.805, 23.0];
j4_y = exp_y(j4_pcb[1]);
j4_w = usb_w;  j4_h = usb_h; // 缩进约 7.5 mm，包胶须整段穿墙，沿用 H1 实测开口
j4_ctr_z = 1.6;       // USB 中心离扩展板顶面，暂定值，装机前实测校准
led_pcb = [[98.5, 30.3], [98.5, 34.2]]; // D8 / D9
led_ys = [for (p = led_pcb) exp_y(p[1])];
led_ctr_z = 0.6;      // LED 发光中心离板顶面，暂定值，支持独立校准
led_window_d = 1.3;   // 统一 USB 轮廓后保留实体；仅观察通道，不是导光柱
led_rim = 0.7;        // 孔周完整实体留边；统一 USB 穿孔到 D8 孔最窄 0.71

/* ---------- 按键（Y+ 壁）：只开 POWER ---------- */
// 实测：中心=玻璃左+31(POWER) / +39(BOOT) / +47(RESET)
btn_power_x = glass_left + 31;     // 22.8，离 SD 最远
// ⚠️ V4.7 起 Y+ 壁这个孔**堵掉**：微雪 Key3 只能关载板自己，而整机 5V 是扩展板
// 升压产的，那颗键够不着总电门。主电源键改为扩展板上的 SW3（见下方 X+ 壁一节）。
// 留着空列表而不是删掉参数：要临时开回 RESET/BOOT 时填 glass_left+47 即可。
btns        = [];
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
// ⚠️ 以上三个数是 Y+ 壁**两次卡尺标定**出来的（见 btn_key_depth 的注释），
// 跨距 3.3mm 专用。X+ 壁的 SW3 跨 4.468mm，**另起一套 sw3_* 参数**，
// 不要为了迁就 SW3 去改这里 —— 改了会让 btn_gap_wc 变负（销子常压按键）。

/* ---------- 按压销通用部分（Y+ 壁与 X+ 壁共用杆径/法兰）---------- */
pl_shaft_d  = 3.7;                 // 按压销杆径
pl_flange_d = 6.2;  pl_flange_t = 1.0;
pl_nose     = 1.0;                 // Y+ 壁顶鼻：够长才不会按到底时法兰撞屏板边缘
pl_proud    = 1.2;                 // Y+ 壁静止时销尾高出外壁量（原 2.0 太凸）
n_plungers  = 4;                   // 独立打印的 SW3 按键销数量（1 用 3 备）

/* ---------- SW3 主电源键（X+ 壁，扩展板上的 TS24CA 侧按键）----------
 *
 * 整机唯一的机械总电门：接 SY6970 的 /QON，按住 1.25~2.25s 开机、12~18s 硬复位，
 * 不依赖任何固件（微雪 Key3 只关得掉载板自己，整机 5V 是扩展板升压产的）。
 * 摆位由 PCB 决定，这里只做坐标换算，**不得另设一套数字**。
 *
 * 与 Y+ 壁的两个本质差别：
 *
 * ① 跨距更大。销子要跨的距离 = 内壁面到按钮尖端：
 *      Y+ 壁 = cav_y1 − btn_key_y = 63.5 − 60.2 = 3.300
 *      X+ 壁 = cav_x1 − sw3_key_x = 106.45 − 101.982 = 4.468   （多 1.168）
 *    采用1.2mm短防呆环，沉腔同时利用墙厚包住法兰；销从盒内穿入。
 *    细鼻仍为1.6mm，后接粗颈维持原静止间隙；按下范围以开关额定行程计算。
 *
 * ② 不确定量的来源不同。Y+ 那颗键在微雪板上、微雪板由玻璃在口内定位，所以键帽
 *    位置带 ±fit_glass 的游动；SW3 在扩展板上，扩展板是**螺丝锁死**的，位置不随
 *    玻璃动，不确定量改为螺丝孔间隙（φ2.9 孔套 M2.5，单边约 0.2mm）。
 *
 * ⚠️ 静止间隙宁大勿小：常压会把 /QON 长期拉低，整机反复重启（TS24CA 行程只有
 *    0.15±0.05，压一点就触发）。所以 gap 取到 0.5 附近，靠 sw3_proud 给够行程，
 *    而不是靠缩小 gap 去省行程。
 */
sw3_pcb = [97.882, 39.1032];        // SW3 在扩展板 PCB 上的位置（KiCad 坐标）
sw3_btn_proud = 2.85;              // 按钮尖端相对元件原点的 X（封装 F.Fab 同值）
sw3_btn_ctr_z = 0.95;              // 实际3D按钮包络Z0.5..1.4的中心；并非用外壳高度相除
sw3_seat_x    = 1.60;              // 座子前表面相对元件原点的 X（封装同值），
                                   // 用来校验顶鼻不会压到座子外壳/固定脚牛角
sw3_travel    = 0.20;              // 按键行程上限（图纸 0.15±0.05）
sw3_fit       = 0.2;               // 扩展板装配游动（螺丝孔间隙单边）
sw3_y     = exp_y(sw3_pcb[1]);                   // = 21.8968
sw3_key_x = exp_x(sw3_pcb[0]) + sw3_btn_proud;   // 键帽面 = 101.982
// sw3_z（孔中心高）依赖 z_exp_top，挪到下面「派生尺寸」段定义，
// 不在这里写 —— 顶层变量先用后定义会静默拿到 undef，孔就悄悄没了。

sw3_d       = btn_d;               // 导孔 φ4.1，与 Y+ 壁同销径
sw3_rec_d   = 6.4;                 // 外侧圆凹避开 D9，导孔/内部导向套尺寸不变
sw3_boss_d  = btn_boss_d;          // 导向套外径 φ9.0
sw3_guide   = 1.2;                 // 防呆环伸入量；沉腔同时利用既有墙厚
sw3_pocket_depth = 2.2;            // φ6.6内侧开放沉腔，包护法兰和正常行程
sw3_rec_depth = 0;                // PWR单独取消外凹，保留0.8mm承拉肩；其它接口不变
sw3_bore_in = sw3_guide-sw3_pocket_depth; // 负值表示导孔起点移入原墙厚
sw3_nose_d  = 1.2;                 // 前端细鼻避开PCB；后部粗颈承担长度
assert(sw3_nose_d>0 && sw3_nose_d/2<sw3_btn_ctr_z,"SW3鼻端半径必须小于按钮中心离PCB高度");
sw3_nose_fine_l = 1.6;            // 细鼻长度不随沉腔加深而变长
sw3_nose    = sw3_nose_fine_l+sw3_pocket_depth; // 后增2.2mm粗颈，保持鼻面102.65
sw3_proud   = 1.6;                 // 外露长度；实际工作由SW3限位，不等于可安全强推行程
side_lbl_z  = 9.4;                 // PWR / USB 共用文字中心高度
sw3_lbl_z   = side_lbl_z;

/* ---------- SD 卡槽（Y+ 壁）---------- */
sd_cx    = glass_left + 59;    // 50.8。实测：卡座中心=玻璃左+59
sd_w     = 16.0;  sd_h = 3.5;
sd_glass_d = 9.35;             // 量尺 z=19.80
sd_zoff  = glass_t + scr_glass_gap + scr_pcb_t - sd_glass_d;      // -1.85
sd_rec   = [22, 8];            // 外侧圆角沉槽（深 rec_depth）
sd_notch = 10;                 // 指甲槽宽（圆角，贯穿余壁）

/* ---------- V4 可拆接口盖：朝天面打印时贴板 ---------- */
top_depth = 20;               // 增深容纳陶瓷空气层、背面元件和可换托盘
top_h = 14;
top_roof_t = 1.4;
cap_fit = 0.3;                // 可拆件横向间隙；承压面接触，不以间隙承载
sma_external_ds = [15,15,30]; // 18/18/30 仅极端包络检查，不等于旋紧手部净空
sma_gps_x = scr_pcb_l+glass_ov_xp+fit_glass+wall_t-corner_r-sma_external_ds[2]/2+.25; // 齐平降孔后保持AIR净距
sma_gps_z = (glass_seat_z+glass_t)/2; // 外部安装包络按整壳厚度居中，包含底板
sma_ports = [[50.25,7.5,sma_external_ds[0]], [68.25,7.5,sma_external_ds[1]], [sma_gps_x,sma_gps_z,sma_external_ds[2]]];
sma_d = 6.6;
sma_cb_d = 9.5;              // 螺母和尾柱最大机械包络，按所购实物校准
sma_nut_af = 8;             // 用户实测六角对边
sma_nut_fit = 0.3;          // 总对边装配余量
sma_nut_depth = 2.2;        // 内加材料容纳止转槽，原1.8穿板面保持
sma_tail_l = 12;
sma_external_l = 30;
cap_high_x = sma_ports[2][0]-sma_ports[2][2]/2-corner_r;
cap_high_h = glass_seat_z+glass_t; // GPS区与主盒外顶齐平，不为φ30外包络凸高
cap_fix_xs = [40,101];        // 中心距61；避开陶瓷仓、三SMA尾柱及两侧墙
cap_fix_front_inset = 15.39;
cap_post_d = 8.5;
cap_base_d = 8.5;
cap_base_h = 4;
cap_screw_d = 2.9;
cap_head_d = 5.8;
cap_head_h = 1.4;
cap_nut_af = 5.2;            // M2.5 内部侧插六角窝；外表面只留圆孔
cap_nut_h = 2.2;
cap_nut_z = 9.2;
cap_screw_tip_clear = 0.6;   // 螺钉端部余量，避免顶住盲孔底而未夹紧
// 两颗 M2.5×10 从底面圆沉孔向上旋入盖内侧插螺母；头/螺母规格须实物核对。
layout_gap = 6;
cap_rib_w = 3;              // 盖柱与朝天面相连的打印支撑肋
// 板间线束示意外径；路径只是机械包络，不代表实测允许弯曲半径。
coax_d = 2;

/* ---------- 内GPS：模块、陶瓷、背面和胶层分开定义 ---------- */
gps_mode = "ceramic";        // ceramic / patch / none；不改变三SMA
patch_cx = 12.75;
gps_patch_size = [40,6,2.3];
gps_patch_z = 7.5;
gps_wall_t = 0.8;
gps_cx = 17;                 // 陶瓷模块中心，与贴片中心独立
gps_board_size = [20,20];     // 完整底板 X/Z 外廓
gps_board_t = 1.0;            // 未实测假设；与背面突出1.6合为2.6，须按模块核对
gps_ceramic_size = [20,20];
gps_ceramic_offset = [0,0];
gps_ceramic_h = 6.6;         // 完整模块总厚=陶瓷4+PCB1+背面1.6；薄款4.6对应陶瓷2
gps_ceramic_cap_t = 1.2;
gps_air_gap = 4;             // 接收面→朝天壁内面的空气层；试验初值，非RF认证
gps_fit = 0.6;
gps_clearance = 10;          // 到PCB/金属的机械预留，不是射频性能保证
gps_back_component_size = [12,12,1.6]; // 未实测的背面元件包络 X/Z/深度
gps_back_component_offset = [0,0];
gps_back_clear = 0.5;
gps_module_capacity_h = 6.6; // 共用仓/托盘接口按最厚模块预算，薄款只改托盘接触柱
gps_glue_t = 0.3;
gps_corner_diag_clear = 24; // 两三角内侧45°切面之间的垂直净距，非板边长
// 三角支撑与PCB的接触区需按实物核对；另一对角不设凸起。
gps_cable_slot_w = 6;       // 原Z+走线边中央豁口宽度，两端保持全高
gps_tray_t = 1.6;
gps_tray_margin = 1.8;
gps_tray_loc_fit = 0.25;     // 托盘到壳体左右止口的单边间隙
gps_foam_t = 1.0;           // 外购软泡棉原厚，盖合压在塑料背框顶边，不能压陶瓷
gps_foam_pad = 4;           // 两处4×4泡棉座，需剪贴泡棉才有预压防晃
gps_bay_h = 28;
preview_hardware = false;  // 仅装配检查时打开；参考体不参与STL导出
explode_distance = 32;

/* ---------- 1/4"-20 云台转接板（外挂盒底，与 4 颗底部螺丝共用）----------
   载荷路径：云台螺柱自板底旋入 → 螺母坐在板内六角腔 → 腔底实体承拉。
   螺母腔开口朝上，装到盒底后被盒体外底面封住，螺母跑不掉。
   转接板用 part="plate" 独立导出；mount_plate 仅控制装配预览。 */
mount_plate = false;
mp_t        = 6.0;    // 板厚
mp_nut_af   = 11.3;   // 1/4-20 螺母对边（标准 11.11 + 0.2 装配间隙）
mp_nut_t    = 3.4;    // 螺母腔深：配 3.2 薄螺母。换 5.56 标准螺母则改 5.8，
                      // 同时 mp_t 加到 8.5、螺丝加长 2.5
mp_stud_d   = 7.0;    // 云台螺柱过孔（φ6.35 + 间隙）
mp_arm_w    = 12;     // 臂宽
mp_pad_d    = 12;     // 螺丝端圆盘直径
mp_hub_d    = 24;     // 中央凸台直径
mp_cb_d     = 5.6;  mp_cb_dp = 1.4;   // 板底沉孔（容螺丝头）

/* ---------- 底面 ---------- */
foot_d = 8;  foot_h = 0;  foot_inset = 5.5;    // 默认平底；需要外凸脚时再设置高度
lighten = true;       // 底板条栅镂空；启用喇叭时整条取消与浅仓重叠的槽
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
top_x1 = box_x1;  // 主壳总宽保持原版118.5，三口与贴片向内重排
top_l = top_x1-box_x0;
box_l = box_x1 - box_x0;   box_w = box_y1 - box_y0;

top_y0 = box_y1;
top_y1 = box_y1 + top_depth;
cap_fix_y = top_y1-cap_fix_front_inset;
top_inner_y1 = top_y1-wall_t;
cap_root_y = cav_y1+cap_fit;
sd_island_x0 = sd_cx-12;
sd_island_x1 = sd_cx+12;
support_islands = [[8,16],[71,79]]; // SD门面随顶盖拆下；主盒只保留两处玻璃支墩
gps_patch_back_y = top_y1-gps_wall_t-gps_patch_size[2];
gps_face_y = top_y1-gps_ceramic_cap_t-gps_air_gap;
gps_ceramic_t = gps_ceramic_h-gps_board_t-gps_back_component_size[2];
gps_back_y = gps_face_y-gps_ceramic_h; // 完整模块最后端，已含背面元件
gps_pcb_back_y = gps_back_y+gps_back_component_size[2];
gps_tray_back_y = gps_face_y-gps_module_capacity_h-gps_back_clear-gps_tray_t;
gps_support_h = gps_pcb_back_y-gps_glue_t-gps_tray_back_y-gps_tray_t;
gps_corner_leg = (gps_board_size[0]+gps_board_size[1])/2-gps_corner_diag_clear/sqrt(2);
gps_cable_notch_h = gps_face_y-gps_tray_back_y-gps_tray_t; // 开到底板上表面，保留完整底板
gps_tray_w = gps_board_size[0]+2*gps_tray_margin;
gps_tray_h = gps_board_size[1]+2*gps_tray_margin;
gps_cz = floor_t+gps_board_size[1]/2+gps_tray_margin;
gps_tray_x0 = gps_cx-gps_tray_w/2;
gps_tray_z0 = floor_t;
gps_foam_xs = [gps_cx-gps_board_size[0]/2+1.5,gps_cx+gps_board_size[0]/2-1.5];
gps_foam_gap = gps_bay_h-top_roof_t-(gps_tray_z0+gps_tray_h);
gps_bay_round_allow = max(0,corner_r-top_roof_t-0.6); // 圆角内肉避开托盘上角
gps_bay_x0 = round(1000*(gps_tray_x0-cap_fit-wall_t/2-gps_bay_round_allow))/1000;
gps_bay_x1 = round(1000*(gps_tray_x0+gps_tray_w+cap_fit+wall_t/2+gps_bay_round_allow))/1000;
// [X左,X右,朝天端高度,是否SD开放操作区]。
cap_segments = gps_mode=="ceramic" ?
    [[box_x0,gps_bay_x0,top_h,false],[gps_bay_x0,gps_bay_x1,gps_bay_h,false],
     [gps_bay_x1,sd_island_x0-cap_fit,top_h,false],
     [sd_island_x0-cap_fit,sd_island_x1+cap_fit,top_h,true],
     [sd_island_x1+cap_fit,cap_high_x,top_h,false],
     [cap_high_x,top_x1,cap_high_h,false]] :
    [[box_x0,sd_island_x0-cap_fit,top_h,false],
     [sd_island_x0-cap_fit,sd_island_x1+cap_fit,top_h,true],
     [sd_island_x1+cap_fit,cap_high_x,top_h,false],
     [cap_high_x,top_x1,cap_high_h,false]];
function cap_height(x) = gps_mode=="ceramic" && x>=gps_bay_x0 && x<=gps_bay_x1 ? gps_bay_h : x>=cap_high_x ? cap_high_h : top_h;
print_y1 = top_y1;
sma_pair_gaps = [for(i=[0:1]) norm([sma_ports[i+1][0]-sma_ports[i][0],sma_ports[i+1][1]-sma_ports[i][1]])-(sma_ports[i+1][2]+sma_ports[i][2])/2];

z_glass_b = glass_seat_z;
z_scr_bot = z_glass_b - glass_to_screen_back;
z_exp_top = z_scr_bot - stack_h + exp_support_adjust;
z_exp_bot = z_exp_top - exp_t;
bat_h = z_exp_bot - floor_t;             // 派生柱高/电池仓高，不能再独立调整
board_gap = z_scr_bot - z_exp_top;
coax_z = (z_exp_top+z_scr_bot)/2;
// ADS-B线先从SD低屋面下绕过左固定柱后侧，再在SD左侧抬入板间。
coax_paths = [[[50.25,top_inner_y1-sma_tail_l,7.5],[50.25,64.5,10],[34,64.5,10],[34,65.5,coax_z],[34,60,coax_z]],
              [[68.25,top_inner_y1-sma_tail_l,7.5],[68.25,top_inner_y1-sma_tail_l-1.2,7.5],[68.25,67,coax_z],[67,60,coax_z]],
              [[sma_gps_x,top_inner_y1-sma_tail_l,sma_gps_z],[sma_gps_x,68,coax_z],[sma_gps_x,60,coax_z]]];
j4_z = z_exp_top + j4_ctr_z;
led_z = z_exp_top + led_ctr_z;
box_h     = z_glass_b + glass_t;          // 口沿与玻璃面齐平
assert(glass_to_screen_back > scr_pcb_t && bat_h > 0 && board_gap >= pillar_len,
       "高度链无效：玻璃/板面/铜柱相交，检查独立校准参数");
assert(abs(bat_h - (z_exp_bot-floor_t)) < 0.001,
       "bat_h是派生柱高，请用exp_support_adjust调整支撑");

// 两个实际插座中心仍各自保留，外墙开口取共同包围框，直边不随上下接口错台。
usb_y0 = min(h1_y-usb_w/2, j4_y-j4_w/2);
usb_y1 = max(h1_y+usb_w/2, j4_y+j4_w/2);
usb_z0 = min(z_scr_bot+usb_zoff-usb_h/2, j4_z-j4_h/2);
usb_z1 = max(z_scr_bot+usb_zoff+usb_h/2, j4_z+j4_h/2);
usb_open_y = (usb_y0+usb_y1)/2;
usb_open_z = (usb_z0+usb_z1)/2;
usb_open_w = usb_y1-usb_y0;
usb_open_h = usb_z1-usb_z0;

hx0 = (scr_pcb_l - hole_dx) / 2;          // 5.25
hy0 = (scr_pcb_w - hole_dy) / 2;          // 5.0
boss_xy = [[hx0, hy0], [hx0 + hole_dx, hy0],
           [hx0, hy0 + hole_dy], [hx0 + hole_dx, hy0 + hole_dy]];

if(speaker_enabled) {
    assert(min(speaker_size)>0 && speaker_fit>=.25 && speaker_tape_t>=.2,
           "喇叭外廓、配合余量或外框胶层无效");
    assert(speaker_recess>0 && floor_t-speaker_recess>=1.2,
           "喇叭浅仓必须保留至少1.2mm周边底厚");
    assert(min(speaker_diaphragm)>0 && speaker_diaphragm[0]<=speaker_size[0]-4 &&
           speaker_diaphragm[1]<=speaker_size[1]-4 && speaker_hole_d>0 &&
           speaker_hole_pitch-speaker_hole_d>=1.2,"出声孔必须留承托边和至少1.2mm网格筋");
    assert(abs(speaker_wire_offset)+speaker_wire_w/2<speaker_size[1]/2-1 && speaker_wire_w>=1.2,
           "喇叭线槽越过外框角或宽度不足");
    assert(speaker_xy[0]-speaker_size[0]/2-speaker_fit>cav_x0+1.2 &&
           speaker_xy[0]+speaker_size[0]/2+speaker_fit<cav_x1-1.2 &&
           speaker_xy[1]-speaker_size[1]/2-speaker_fit>cav_y0+1.2 &&
           speaker_xy[1]+speaker_size[1]/2+speaker_fit<cav_y1-1.2,"喇叭浅仓越过主盒底板");
    for(p=boss_xy) assert(norm([max(0,abs(p[0]-speaker_xy[0])-speaker_size[0]/2-speaker_fit),
                               max(0,abs(p[1]-speaker_xy[1])-speaker_size[1]/2-speaker_fit)])>boss_d/2+1,
                          "喇叭浅仓侵入螺柱或柱根留边");
    assert(speaker_z+speaker_size[2]+2<z_exp_bot,"喇叭背面到扩展板不足2mm，需核对下伸器件/焊脚");
}

echo(str("盒体含顶舱外形: ", top_l, " x ", top_y1-box_y0, " x ", max(box_h,cap_high_h,gps_mode=="ceramic"?gps_bay_h:0), " mm；主壳Z=",box_h));
echo(str("扩展板底面 z=", z_exp_bot, "  屏板底面 z=", z_scr_bot,
         "  玻璃底 z=", z_glass_b));
mp_cx = (box_x0 + box_x1) / 2;
mp_cy = (box_y0 + box_y1) / 2;
assert(mp_t - mp_nut_t >= 2.0, "转接板螺母腔下方肉太薄：加大 mp_t 或减小 mp_nut_t");
echo(str("云台板: ", mp_t, " 厚  1/4-20 螺母腔 对边", mp_nut_af, " 深", mp_nut_t,
         "  腔底承拉肉 ", mp_t - mp_nut_t,
         "  装板后总高 ", box_h + mp_t,
         "  螺丝需 ≈ ", mp_t - mp_cb_dp + floor_t + bat_h + exp_t
                        + (board_gap - pillar_len) + 3));
echo(str("螺丝参考长度 ≈ ",
         floor_t - cb_depth + bat_h + exp_t + (board_gap - pillar_len) + 3,
         " mm（含悬空 ", board_gap - pillar_len, " + 旋入铜柱 3）"));
echo(str("玻璃基准→屏PCB背面 ", glass_to_screen_back, "，板间净距 ", board_gap,
         "，支撑修正 ", exp_support_adjust, "（默认尺寸未代表实物插合已验证）"));
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
// V4.7 起 btns 默认为空（Y+ 壁不开孔），这组校验只在真的开孔时才有意义；
// 条件化而不是删掉，是为了把 RESET/BOOT 填回 btns 时护栏立刻回来。
btn_on = len(btns) > 0;
assert(!btn_on || btn_gap_wc > 0.1, "玻璃偏向按键壁时销子会预压按键：减小 pl_nose");
assert(!btn_on || btn_travel < pl_proud, "行程不够：加大 pl_proud");
assert(!btn_on || cav_y1 - btn_bore_in - pl_flange_t - btn_travel > scr_pcb_w + 0.3,
       "按到底时法兰会撞屏板边缘：加大 pl_nose");
assert(!btn_on || cav_y1 - btn_guide > btn_key_y + 1.0, "导向套会顶到键帽：缩短 btn_guide");

/* ---------- SW3 主电源键几何自检（X+ 壁）----------
   不确定量来自扩展板螺丝孔间隙 ±sw3_fit（不是玻璃游动），两端都要校验。
   ⚠️ 判死的一条是 sw3_gap_wc：它一旦 ≤0，销子就常压 /QON，整机反复重启。 */
sw3_z      = z_exp_top + sw3_btn_ctr_z;                            // 15.95
sw3_span   = cav_x1 - sw3_key_x;                                   // 跨距 4.468
sw3_tip_x  = cav_x1 - sw3_bore_in - pl_flange_t - sw3_nose;        // 顶鼻静止位
sw3_gap    = sw3_tip_x - sw3_key_x;
sw3_gap_wc = sw3_tip_x - (sw3_key_x + sw3_fit);   // 板偏向壁：最易预压
sw3_gap_bc = sw3_tip_x - (sw3_key_x - sw3_fit);   // 板偏离壁：最费行程
sw3_need   = sw3_gap_bc + sw3_travel + 0.15;      // 走完空隙 + 压到底 + 余量
sw3_exp_x1 = exp_x(exp_l);                        // 扩展板 X+ 板边 = 101.25
sw3_seat_face = exp_x(sw3_pcb[0]) + sw3_seat_x;   // 座子前表面 = 100.732
// φ4.1杆孔与φ6.6法兰腔共同限倾；静位肩面限制倾斜，按下后支点跨距增加。
// 取两支点径向间隙之和作为宽松角界，忽略斜截面扩张；真实工作仍由SW3限位。
sw3_work_stroke = sw3_gap_bc+sw3_travel;
sw3_bore_l = wall_t+sw3_bore_in-sw3_rec_depth;
sw3_radial_play = ((sw3_d-pl_shaft_d)+(btn_pkt_d-pl_flange_d))/2;
function sw3_tilt_at(stroke) = min(asin(min(1,stroke/(pl_flange_d/2))),
    atan(sw3_radial_play/(sw3_bore_l+pl_flange_t+stroke)));
sw3_tilt_bound = atan(sw3_radial_play/(sw3_bore_l+pl_flange_t+sw3_gap_wc));
sw3_work_pcb_gap = sw3_tip_x-sw3_work_stroke-sw3_nose_d/2*sin(sw3_tilt_bound)-(sw3_exp_x1+sw3_fit);
sw3_nose_swing = (btn_pkt_d-pl_flange_d)/2+sw3_nose*tan(sw3_tilt_bound);
sw3_button_overlap = .45+sw3_nose_d/2*cos(sw3_tilt_bound)-sw3_nose_swing;
assert(sw3_bore_l>0 && sw3_work_pcb_gap>0 && sw3_button_overlap>0,"嵌入式按键工作包络无效：销鼻越板边或偏离按钮");
echo(str("SW3内凸=",sw3_guide,"mm，沉腔深=",sw3_pocket_depth,"mm，承拉肩=",sw3_bore_l,
    "mm；额定工作推进≤",sw3_work_stroke,"mm，接触后倾角宽松界=",sw3_tilt_bound,
    "°，PCB轴向余量≥",sw3_work_pcb_gap,"mm；未装板时仍可从内侧取出，不是双向锁扣。"));
echo(str("SW3: 孔中心 y=", sw3_y, " z=", sw3_z, "  跨距 ", sw3_span,
         "  扣外凹后导向长 ", sw3_bore_l, "  静止间隙 ", sw3_gap,
         "（最坏 ", sw3_gap_wc, "）  含余量所需外露 ", sw3_need, " / 实际外露 ", sw3_proud,
         "  销总长 ", sw3_nose + pl_flange_t + wall_t + sw3_bore_in + sw3_proud));
assert(sw3_gap_wc > 0.1,
       "扩展板偏向 USB 壁时销子会常压 SW3（/QON 拉低→反复重启）：减小 sw3_nose");
assert(sw3_need < sw3_proud, "SW3 行程不够：加大 sw3_proud");
assert(sw3_pocket_depth>=pl_flange_t+sw3_work_stroke+.1,"沉腔须容纳法兰及正常工作行程");
assert(sw3_bore_l>=.6 && pl_flange_d>sw3_d+.6,"沉腔外侧承拉肩不足或法兰不能止退");
assert(cav_x1 - sw3_bore_in - pl_flange_t - sw3_need > sw3_exp_x1 + 0.3,
       "按到底时法兰会撞扩展板边缘：加大 sw3_nose");
assert(sw3_key_x - sw3_travel > sw3_seat_face + 0.3,
       "顶鼻会压到 TS24CA 座子外壳/固定脚牛角而不是按钮");
assert(cav_x1 - sw3_guide > sw3_exp_x1 + 0.3, "导向套会撞扩展板边缘：缩短 sw3_guide");
assert(sw3_z + sw3_boss_d / 2 < z_scr_bot - 0.3, "导向套顶会撞屏板底面：SW3 太高");
assert(abs(sw3_y - h1_y) > (sw3_rec_d + usb_rec[0]) / 2 + 0.5,
       "SW3 外凹与 H1 USB 沉槽在 X+ 壁上重叠");
// 圆角矩形有符号距离：校准接口高度/尺寸时，检查穿孔仍给 LED 留足实体。
function slot_distance(y, z, cy, cz, w, h, rounding=slot_r) =
    let(r = min(rounding, min(h, w)/2 - 0.01),
        q = [abs(y-cy)-w/2+r, abs(z-cz)-h/2+r])
    norm([max(q[0], 0), max(q[1], 0)]) + min(max(q), 0) - r;
assert(led_rim >= 0.7 && led_window_d > 0, "LED 观察孔必须有至少 0.7 mm 留边");
for (ly = led_ys) {
    assert(slot_distance(ly, led_z, usb_open_y, usb_open_z, usb_open_w, usb_open_h) >= led_window_d/2+led_rim,
           "USB 穿孔侵入 LED 留边：检查实测接口尺寸/高度");
    assert(norm([ly-sw3_y, led_z-sw3_z]) >= (sw3_rec_d+led_window_d)/2+led_rim,
           "电源键外凹侵入 LED 留边：缩小 sw3_rec_d");
}
// 「PWR 凹字不压进 SW3 外凹槽」那条断言依赖 emb_sz，放在它定义之后（见 labels 段前）。

/* ---------- 分件与真实包络约束 ---------- */
sd_antenna_gap = z_scr_bot+sd_zoff-sd_h/2-(sma_ports[0][1]+sma_ports[0][2]/2);
assert(gps_mode=="ceramic" || gps_mode=="patch" || gps_mode=="none", "未知gps_mode");
assert(cap_high_h<=box_h+.000001 && abs(sma_ports[2][1]-box_h/2)<.000001,"GPS安装区须齐平并按整壳外厚居中");
// φ30是外部天线包络，默认上下各外伸0.425；螺母承压及内部包络另行校验。
assert(len(sma_ports)==3 && len(sma_external_ds)==3 && min(sma_external_ds)>0, "三个SMA外廓必须有效");
assert(min(sma_pair_gaps)>0.8 && sd_antenna_gap>=1, "外接天线互撞或侵入SD通道");
assert(top_inner_y1-sma_tail_l>exp_y(0)+0.5, "SMA尾柱进入板框");
assert(cap_nut_af>cap_screw_d && cap_base_h-cap_head_h>=1.5, "底座圆沉孔承压肩不足");
assert(cap_nut_z>cap_base_h+2 && cap_nut_z+cap_nut_h<top_h-top_roof_t, "内部螺母座承压链无效");
for(p=sma_ports) {
    assert(p[1]-sma_cb_d/2>=floor_t+0.25, "SMA螺母碰底托");
    assert(p[1]+sma_cb_d/2<=cap_height(p[0])-top_roof_t-0.25, "SMA螺母碰盖顶");
    assert(p[0]+sma_cb_d/2<cav_x1 && p[0]-sma_cb_d/2>cav_x0, "SMA螺母碰侧壁");
    // 固定座在尾柱后端之前；按Y间距计入圆柱横向收缩，而非只比X。
    for(x=cap_fix_xs) assert(abs(x-p[0])>sma_cb_d/2+sqrt(max(0,pow(cap_post_d/2,2)-pow(max(0,top_inner_y1-sma_tail_l-cap_fix_y),2)))+.3,
                           "紧固座与SMA尾柱包络间距不足");
}
assert(patch_cx-gps_patch_size[0]/2-gps_fit>=cav_x0, "贴片薄壁窗口越过侧壁");
assert(sma_ports[0][0]-sma_external_ds[0]/2-patch_cx-gps_patch_size[0]/2>=gps_clearance,
       "贴片到外接天线保守包络不足10mm");
if(gps_mode=="ceramic") {
    assert(gps_foam_gap>0 && gps_foam_t>gps_foam_gap && gps_foam_t-gps_foam_gap<=0.6,
           "泡棉须适量压缩而非硬顶陶瓷仓");
    assert(gps_ceramic_t>0 && gps_air_gap>=0 && gps_glue_t>=0, "模块厚/空气层/胶厚无效");
    assert(gps_ceramic_h<=gps_module_capacity_h && gps_support_h>0, "模块超出共用仓容量，需调整容量并重打仓体");
    assert(gps_tray_back_y-exp_y(0)>=gps_clearance, "陶瓷托盘侵入板边10mm预留：加深舱体");
    assert(gps_tray_z0>=floor_t && gps_tray_z0+gps_tray_h<=gps_bay_h-top_roof_t-cap_fit, "陶瓷托盘越过底/顶");
    assert(gps_bay_x0>box_x0 && gps_bay_x1<sd_island_x0-0.5, "陶瓷仓越过走线区");
    assert(gps_ceramic_cap_t>=1 && gps_tray_t>=1.2 && gps_corner_leg>0 && gps_corner_leg<min(gps_board_size)/2, "陶瓷壁/背托无效");
    assert(abs(gps_ceramic_offset[0])+gps_ceramic_size[0]/2<=gps_board_size[0]/2 &&
           abs(gps_ceramic_offset[1])+gps_ceramic_size[1]/2<=gps_board_size[1]/2, "陶瓷超出托盘模块外廓");
    assert(abs(gps_back_component_offset[0]+gps_back_component_offset[1])+
           (gps_back_component_size[0]+gps_back_component_size[1])/2+2*gps_back_clear<gps_corner_diag_clear/sqrt(2),
           "三角支撑压住背面元件/馈点包络");
    assert(gps_cable_notch_h>0, "出线豁口须高于底板上表面");
    assert(gps_cable_slot_w>0 && gps_cable_slot_w<gps_board_size[0]-2*gps_corner_leg,
           "中央走线豁口不能侵入两端三角支撑区");
}
assert(coax_z-coax_d/2>z_exp_top+0.25 && coax_z+coax_d/2<z_scr_bot-0.25,
       "线束包络不能进入板间净距");
assert(len(btns)==0,"此版开放顶仓不兼容旧Y+按键；保留X+ SW3。请勿填入btns造成悬空导套");
assert(top_y1-cap_root_y>=box_h-top_h,"盖后坡长度不足，不能保持45度打印坡");
assert(layout_gap>=4,"排版间距至少4mm");
echo(str("V4 顶舱Y延伸=",top_depth," 主墙厚=",wall_t," GPS区X/Z=",[cap_high_x,top_x1,floor_t,cap_high_h]," GPS孔中心XZ=",[sma_ports[2][0],sma_ports[2][1]]));
echo(str("V4 SMA内止转槽AF=",sma_nut_af+sma_nut_fit," 深=",sma_nut_depth," 保留穿板厚=",wall_t));
echo(str("V4 SMA外廓间隙=",sma_pair_gaps," SD直线余量=",sd_antenna_gap));
echo(str("V4陶瓷空气层=",gps_air_gap," 背面y=",gps_back_y," 托盘背面y=",gps_tray_back_y,
         "；背面元件/接触点尚未实测，须核对后使用胶固定"));

/* ---------- 基础形 ---------- */
module rrect(l, w, r) { offset(r = r) offset(r = -r) square([l, w]); }

module outer_outline() {
    // 底板和顶盖共用完整R3圆角；朝打印板的圆弧由切片支撑承托。
    translate([box_x0,box_y0]) rrect(box_l,top_y1-box_y0,corner_r);
}

module box_shell() {
    union() {
        // 玻璃原圆角口与顶舱开放区一次定义内腔，避免把上角延成长直边。
        // 底/壁共用同一外体，不另外拼补相切圆角。
        difference() {
            linear_extrude(box_h) outer_outline();
            translate([cav_x0,cav_y0,floor_t]) linear_extrude(box_h-floor_t+.02)
                let(r=glass_r+fit_glass,w=cav_x1-cav_x0,h=cav_y1-cav_y0,
                    t=top_inner_y1-cav_y0,step=360/$fn)
                polygon(concat(
                    [for(a=[180:step:270]) [r+r*cos(a),r+r*sin(a)]],
                    [for(a=[270:step:360]) [w-r+r*cos(a),r+r*sin(a)]],
                    [for(a=[0:step:90]) [w-r+r*cos(a),h-r+r*sin(a)]],
                    [[w,h],[w,t],[0,t],[0,h]],
                    [for(a=[90:step:180]) [r+r*cos(a),h-r+r*sin(a)]]));
            translate([box_x0-1,cav_y1,floor_t])
                cube([box_l+2,top_y1-cav_y1+1,box_h-floor_t+.02]);
        }
        // 支墩全部落到底板，不留跨越开放仓口的悬空上梁。
        for(p=support_islands) translate([p[0],cav_y1,floor_t-0.01])
            cube([p[1]-p[0],wall_t,box_h-floor_t+0.01]);
        // 内侧短导轨遮住侧墙配合缝，落地打印；盖从上方放入。
        for(x=[cav_x0+cap_fit,cav_x1-cap_fit-1.4])
            translate([x,cav_y1-2,floor_t-0.01]) cube([1.4,6,17.6]);
        for(x=[cav_x0,cav_x1-1.7])
            translate([x,cav_y1-2,floor_t-0.01]) cube([1.7,1.8,17.6]);
    }
}

/* ---------- V4 固定座、可换托盘止口；底板属于统一外轮廓 ---------- */
module base_extension() {
    for(x=cap_fix_xs) translate([x,cap_fix_y,0]) cylinder(d=cap_base_d,h=cap_base_h);
    if(gps_mode=="ceramic") {
        for(y=[gps_tray_back_y-cap_fit-1.4,gps_face_y+cap_fit])
            translate([gps_tray_x0,y,floor_t]) cube([gps_tray_w,1.4,1.4]);
        // 左右短止口配合前后原止口；上方泡棉提供预压，干装间隙不等于防晃固定。
        for(x=[gps_tray_x0-gps_tray_loc_fit-1.2,gps_tray_x0+gps_tray_w+gps_tray_loc_fit])
            translate([x,gps_tray_back_y+1,floor_t-.01]) cube([1.2,4,2.41]);
    }
}
module cap_fastener_base_cuts() {
    for(x=cap_fix_xs) translate([x,cap_fix_y,-0.01]) {
        cylinder(d=cap_screw_d,h=cap_base_h+0.02);
        cylinder(d=cap_head_d,h=cap_head_h+0.01);
    }
}
// X方向挤出YZ截面。盖外朝天面向下打印，后坡每层横移不超过层高。
module cap_yz(x,w,points) {
    n=len(points);
    polyhedron(points=concat([for(p=points) [x,p[0],p[1]]],[for(p=points) [x+w,p[0],p[1]]]),
               faces=concat([[for(i=[0:n-1]) i]],[[for(i=[n-1:-1:0]) n+i]],
                            [for(i=[0:n-1]) [i+n,(i+1)%n+n,(i+1)%n,i]]));
}
function segment_ramp_y(s) = s[2]>box_h ? box_y1+cap_fit : cap_root_y;
function segment_bend_y(s) = s[3] ? cap_root_y : segment_ramp_y(s)+abs(box_h-s[2]);
function segment_top(s,y) = s[3] ? s[2] : s[2]>box_h ? min(s[2],box_h+max(0,y-segment_ramp_y(s))) : max(s[2],box_h-(y-cap_root_y));
module cap_volume(s,x,w,drop=0) {
    bend = segment_bend_y(s);
    cap_yz(x,w,concat([[cap_root_y,-1],[top_y1+0.02,-1],[top_y1+0.02,s[2]-drop],[bend,s[2]-drop]],
             s[2]>box_h?[[segment_ramp_y(s),box_h-drop]]:[],
             [[cap_root_y,segment_top(s,cap_root_y)-drop]]));
}
module cap_roof(s) {
    bend = segment_bend_y(s);
    cap_yz(s[0],s[1]-s[0],concat([[cap_root_y,segment_top(s,cap_root_y)-top_roof_t]],
        s[2]>box_h?[[segment_ramp_y(s),box_h-top_roof_t]]:[],
        [[bend,s[2]-top_roof_t],[top_y1+0.02,s[2]-top_roof_t],[top_y1+0.02,s[2]],[bend,s[2]]],
        s[2]>box_h?[[segment_ramp_y(s),box_h]]:[],[[cap_root_y,segment_top(s,cap_root_y)]]));
}
// 圆角跟随YZ坡面扫掠；半径从0平滑增至corner_r，避免圆柱硬截断的方口。
// 过渡长2R，使半径始终不大于相邻坡面高差，完整保留原屋面接合厚度。
function cap_corner_start(s) = s[2]<box_h ? segment_bend_y(s) : cap_root_y;
function cap_corner_radius(s,y) = let(u=min(1,max(0,(y-cap_corner_start(s))/(2*corner_r))))
    corner_r*u*u*(3-2*u);
function cap_corner_profile(s,side,y,add) =
    let(r=cap_corner_radius(s,y),ri=max(.001,r-top_roof_t-.02),
        edge=side?s[1]:s[0],dir=side?-1:1,zt=segment_top(s,y),
        shape=add ? concat([[0,zt-r],[0,zt],[r,zt]],
            [for(j=[0:15]) [r-ri*sin(j*90/15),zt-r+ri*cos(j*90/15)]]) :
            concat([[-.02,zt+.02],[-.02,zt-r]],
                [for(j=[0:15]) [r-r*cos(j*90/15),zt-r+r*sin(j*90/15)]],[[r,zt+.02]]),
        points=[for(p=shape) [edge+dir*p[0],y,p[1]]])
    ((side==1)!=add) ? [for(j=[len(points)-1:-1:0]) points[j]] : points;
module cap_corner_loft(profiles,tip=undef) {
    n=len(profiles[0]); m=len(profiles);
    polyhedron(points=concat([for(p=profiles) each p],is_undef(tip)?[]:[tip]),
        faces=concat(is_undef(tip)?[[for(j=[n-1:-1:0]) j]]:
                     [for(j=[0:n-1]) [m*n,(j+1)%n,j]],
            [[for(j=[0:n-1]) (m-1)*n+j]],
            [for(k=[0:m-2]) for(j=[0:n-1]) each
                [[k*n+j,k*n+(j+1)%n,(k+1)*n+(j+1)%n],
                 [k*n+j,(k+1)*n+(j+1)%n,(k+1)*n+j]]]));
}
module cap_step_rounding(add=false) {
    for(s=cap_segments) if(s[2]>top_h) {
        start=cap_corner_start(s); end=top_y1+.02;
        // 加入屋面折点，避免用跨越折点的弦去切穿原坡面。
        ys=concat([for(k=[0:ceil((end-start)/.2)-1])
            let(a=start+k*.2,b=min(end,a+.2)) each
            concat([a],segment_ramp_y(s)>a && segment_ramp_y(s)<b?[segment_ramp_y(s)]:[],
                         segment_bend_y(s)>a && segment_bend_y(s)<b?[segment_bend_y(s)]:[])],[end]);
        for(side=[0,1]) let(profiles=[for(y=ys)
            if(add?cap_corner_radius(s,y)>top_roof_t: y>start)
                cap_corner_profile(s,side,y,add)])
            cap_corner_loft(profiles,add?undef:[side?s[1]:s[0],start,segment_top(s,start)]);
    }
}

module cap_body() {
    difference() {
        intersection() {
            union() {
                for(s=cap_segments) {
                    translate([s[0],top_inner_y1,floor_t]) cube([s[1]-s[0],wall_t+.02,s[2]-floor_t-top_roof_t/2]);
                    cap_roof(s);
                    // 后缘短唇止于玻璃外侧，留出竖直提盖路径，不伸到玻璃投影下。
                    if(!s[3]) let(l=max(s[0],cav_x0+cap_fit),r=min(s[1],cav_x1-cap_fit))
                        translate([l,cav_y1,z_glass_b-1.1])
                            cube([r-l,cap_root_y+0.8-cav_y1,0.8]);
                }
                cap_volume(cap_segments[0],box_x0,wall_t);
                cap_volume(cap_segments[len(cap_segments)-1],box_x1-wall_t,wall_t);
                for(i=[1:len(cap_segments)-1]) let(a=cap_segments[i-1],b=cap_segments[i],
                    high=a[3]?b:b[3]?a:a[2]>b[2]?a:b,
                    low=a[3]?a:b[3]?b:a[2]>b[2]?b:a, wx=high==b?b[0]:b[0]-wall_t)
                    difference() {
                        cap_volume(high,wx,wall_t);
                        cap_volume(low,wx-0.01,wall_t+0.02,top_roof_t);
                    }
                cap_step_rounding(true);
                sd_cap_facade();
                if(gps_mode=="ceramic") gps_cap_tray_channel();
                // 内六角止转座向内增厚，穿板承压面仍为wall_t。
                for(p=sma_ports) translate([p[0],top_inner_y1-sma_nut_depth,p[1]])
                    rotate([-90,0,0]) cylinder(d=(sma_nut_af+sma_nut_fit)/cos(30)+2,h=sma_nut_depth+0.01,$fn=6);
                for(x=cap_fix_xs) {
                    translate([x,cap_fix_y,cap_base_h]) cylinder(d=cap_post_d,h=top_h-cap_base_h-top_roof_t/2);
                    translate([x-cap_rib_w/2,cap_fix_y,cap_base_h])
                        cube([cap_rib_w,top_inner_y1-cap_fix_y+0.02,top_h-cap_base_h-top_roof_t/2]);
                }
            }
            translate([0,0,floor_t]) linear_extrude(max(box_h,cap_high_h,gps_bay_h)-floor_t+0.01) outer_outline();
        }
        cap_step_rounding(false);
        sd_cutouts();
        if(labels_enabled) wall_text(lbl_sd,sd_cx,sd_lbl_z,0);
        // 装配避让被主盒实体填满，位置在内部接合处，不作外露走线豁口。
        for(p=support_islands) translate([p[0]-cap_fit,cap_root_y-1,0])
            cube([p[1]-p[0]+2*cap_fit,box_y1+cap_fit-cap_root_y+1,max(box_h,cap_high_h,gps_bay_h)+1]);
        for(p=sma_ports) {
            translate([p[0],top_inner_y1-sma_nut_depth-0.01,p[1]]) rotate([-90,0,0]) cylinder(d=sma_d,h=wall_t+sma_nut_depth+0.02);
            translate([p[0],top_inner_y1-sma_nut_depth-0.01,p[1]]) rotate([-90,0,0])
                cylinder(d=(sma_nut_af+sma_nut_fit)/cos(30),h=sma_nut_depth+0.01,$fn=6);
        }
        for(x=cap_fix_xs) translate([x,cap_fix_y,0]) {
            cylinder(d=cap_screw_d,h=cap_nut_z+cap_nut_h+cap_screw_tip_clear);
            translate([0,0,cap_nut_z]) cylinder(d=cap_nut_af/cos(30),h=cap_nut_h,$fn=6);
            // 螺母从-Y内侧横向插入，外顶面无孔，螺钉从底部进入。
            translate([-cap_nut_af/cos(30)/2,-cap_post_d,cap_nut_z]) cube([cap_nut_af/cos(30),cap_post_d,cap_nut_h]);
        }
        if(gps_mode=="patch") translate([patch_cx-gps_patch_size[0]/2-gps_fit,top_inner_y1-0.01,gps_patch_z-gps_patch_size[1]/2-gps_fit])
            cube([gps_patch_size[0]+2*gps_fit,wall_t-gps_wall_t+0.01,gps_patch_size[1]+2*gps_fit]);
        if(gps_mode=="ceramic") translate([gps_cx-gps_board_size[0]/2-gps_fit,top_inner_y1-0.01,gps_cz-gps_board_size[1]/2-gps_fit])
            cube([gps_board_size[0]+2*gps_fit,wall_t-gps_ceramic_cap_t+0.01,gps_board_size[1]+2*gps_fit]);
        if(labels_enabled) for(i=[0:2]) cap_label(i);
    }
}
// 托盘上沿嵌入四面限位、向下开口的槽；拆盖先抬2mm释放上沿，再向外移。
// 槽壁连到屋面/朝天壁；前后挡都保留，泡棉只消隙，不承担防倒定位。
module gps_cap_tray_channel() {
    z0=gps_tray_z0+gps_tray_h-1.4;
    z1=gps_bay_h-top_roof_t+.2;
    y0=gps_tray_back_y-cap_fit-1.2;
    yf=gps_face_y+cap_fit;
    for(x=[gps_tray_x0-gps_tray_loc_fit-1.2,gps_tray_x0+gps_tray_w+gps_tray_loc_fit])
        translate([x,y0,z0]) cube([1.2,top_inner_y1-y0+.02,z1-z0]);
    translate([gps_tray_x0-gps_tray_loc_fit,yf,z0])
        cube([gps_tray_w+2*gps_tray_loc_fit,top_inner_y1-yf+.02,z1-z0]);
    translate([gps_tray_x0-gps_tray_loc_fit,y0,z0])
        cube([gps_tray_w+2*gps_tray_loc_fit,1.2,z1-z0]);
}
// SD门面从盖的低屋面接起；按用户要求不另加两条朝天侧斜肋。
// 平放打印时约24.6mm横跨；按用户要求不附带内置打印支撑。
module sd_cap_facade() {
    x0=sd_island_x0-cap_fit-.02;
    x1=sd_island_x1+cap_fit+.02;
    translate([x0,cav_y1,top_h-top_roof_t])
        cube([x1-x0,wall_t,box_h-top_h+top_roof_t]);
}
module coax_envelope() {
    for(path=coax_paths) for(i=[0:len(path)-2]) hull() {
        translate(path[i]) sphere(d=coax_d,$fn=16);
        translate(path[i+1]) sphere(d=coax_d,$fn=16);
    }
}
module cap_label(i) {
    p=sma_ports[i];
    if(i==2) // GPS保留孔上方的原位置，仅转向屏幕侧。
        wall_text(lbl_sma[i],p[0],p[1]+sma_d/2+emb_sz/2+.3,2);
    else {
        m=sma_label_metrics(lbl_sma[i]);
        // 实际字框右缘距孔边1.2mm，字脚朝屏幕Z+。
        translate([p[0]-sma_d/2-emb_sma_gap,top_y1-emb_h,p[1]]) rotate([-90,0,0])
            linear_extrude(emb_h+emb_cut)
                translate([-m.position.x-m.size.x,-m.position.y-m.size.y/2])
                    text(lbl_sma[i],size=emb_sz,font=emb_font,spacing=sma_label_spacing);
    }
}
// 装配后的压缩泡棉包络，仅检查/参考，不属于打印件。
module gps_foam_envelope() {
    for(x=gps_foam_xs)
        translate([x-gps_foam_pad/2,gps_tray_back_y,gps_tray_z0+gps_tray_h])
            cube([gps_foam_pad,gps_foam_pad,gps_foam_gap]);
}
// 沿Y挤出同一对三角形，用于实际支撑和胶层参考，避免预览与打印件不一致。
module gps_corner_contacts(y0,h) {
    translate([gps_cx,y0+h,gps_cz]) rotate([90,0,0]) linear_extrude(h)
        for(sign=[-1,1]) scale([sign,sign])
            polygon([[gps_board_size[0]/2,gps_board_size[1]/2],
                     [gps_board_size[0]/2-gps_corner_leg,gps_board_size[1]/2],
                     [gps_board_size[0]/2,gps_board_size[1]/2-gps_corner_leg]]);
}
module gps_tray_body() {
    // 打印件只在允许接触点托住PCB背面；中央完全敞开，避让元件/馈点。
    // 原Z+走线边两端为完整护栏，仅中央开局部豁口；其余三边完整。
    union() {
        difference() {
            translate([gps_tray_x0,gps_tray_back_y,gps_tray_z0]) cube([gps_tray_w,gps_tray_t,gps_tray_h]);
            translate([gps_cx-gps_board_size[0]/2+3,gps_tray_back_y-0.01,gps_cz-gps_board_size[1]/2+3])
                cube([gps_board_size[0]-6,gps_tray_t+0.02,gps_board_size[1]-6]);
        }
        gps_corner_contacts(gps_tray_back_y+gps_tray_t-.01,gps_support_h+.01);
        // 侧向定位条只碰底板侧边方向，不跨接收面；0.6配合量由胶点定位。
        for(x=[gps_tray_x0,gps_cx+gps_board_size[0]/2+gps_fit])
            translate([x,gps_tray_back_y,gps_tray_z0])
                cube([gps_tray_margin-gps_fit,gps_face_y-gps_tray_back_y,gps_tray_h]);
        translate([gps_tray_x0,gps_tray_back_y,gps_tray_z0])
            cube([gps_tray_w,gps_face_y-gps_tray_back_y,gps_tray_margin-gps_fit]);
        // 泡棉座只延伸塑料背框上边，位于PCB上方，不触及陶瓷或背部元件。
        for(x=gps_foam_xs)
            translate([x-gps_foam_pad/2,gps_tray_back_y,gps_tray_z0+gps_tray_h-1.2])
                cube([gps_foam_pad,gps_foam_pad,1.2]);
        // 保留原出线边两端，只有中间6mm宽处开到底板上表面。
        difference() {
            translate([gps_tray_x0,gps_tray_back_y,gps_tray_z0+gps_tray_h-(gps_tray_margin-gps_fit)])
                cube([gps_tray_w,gps_face_y-gps_tray_back_y,gps_tray_margin-gps_fit]);
            translate([gps_cx-gps_cable_slot_w/2,gps_tray_back_y+gps_tray_t,
                       gps_tray_z0+gps_tray_h-(gps_tray_margin-gps_fit)-.01])
                cube([gps_cable_slot_w,gps_cable_notch_h+.02,gps_tray_margin-gps_fit+.02]);
        }
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
    // PWR侧靠排线的舌移至同侧下螺柱Y基准，8mm缩窄30%至5.6mm。
    wedge_xz(boss_xy[1][1]-tab_w*.7/2,tab_w*.7,cav_x1-tab_p,cav_x1+.01,zt,zb);
    wedge_xz(50-tab_w/2,tab_w,cav_x1-tab_p,cav_x1+.01,zt,zb);
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

module usb_recess() {
    // 双口统一圆角矩形浅槽；LED 处保留实体岸，避免孔与浅槽边界粘连。
    difference() {
        slot_cut_x(usb_open_y, usb_open_z, usb_open_w+usb_rec[0]-usb_w,
                   usb_open_h+usb_rec[1]-usb_h, box_x1-rec_depth, rec_depth+1);
        for (ly = led_ys)
            translate([box_x1-rec_depth-0.01, ly, led_z]) rotate([0, 90, 0])
                cylinder(d=led_window_d+2*led_rim, h=rec_depth+1.02);
    }
}

module wall_cutouts() {
    /* --- H1 Type-C（X+）：穿墙槽 + 外侧浅沉槽 --- */
    slot_cut_x(usb_open_y, usb_open_z, usb_open_w, usb_open_h, cav_x1 - 1, cut_len);
    usb_recess();

    // 两个 USB 包胶的原窗口均被统一开孔覆盖。
    for (ly = led_ys)
        translate([cav_x1 - 1, ly, led_z]) rotate([0, 90, 0])
            cylinder(d = led_window_d, h = cut_len);

    /* --- POWER 按键（Y+）：长导向孔 + 套内法兰沉腔 --- */
    z_btn = z_scr_bot + btn_zoff;
    for (bx = btns) {
        slot_cut_y(bx, z_btn, btn_d, btn_d, cav_y1 - btn_bore_in, cut_len);
        translate([bx, cav_y1 - btn_guide - 0.5, z_btn]) rotate([-90, 0, 0])
            cylinder(d = btn_pkt_d, h = btn_guide - btn_bore_in + 0.5);
        translate([bx, box_y1 - rec_depth, z_btn]) rotate([-90, 0, 0])
            cylinder(d = btn_rec_d, h = rec_depth + 0.01);
    }

    /* --- SW3 主电源键（X+）：短凸环+墙内沉腔；从内侧嵌入法兰 --- */
    slot_cut_x(sw3_y, sw3_z, sw3_d, sw3_d, cav_x1 - sw3_bore_in, cut_len);
    translate([cav_x1-sw3_guide-.5,sw3_y,sw3_z])rotate([0,90,0])
        cylinder(d=btn_pkt_d,h=sw3_pocket_depth+.5);
    if(sw3_rec_depth>0) translate([box_x1-sw3_rec_depth,sw3_y,sw3_z])rotate([0,90,0])
        cylinder(d=sw3_rec_d,h=sw3_rec_depth+.01);

}

// SD孔、浅沉槽和标字跟随顶盖，不能再从主盒切削。
module sd_cutouts() {
    z_sd = z_scr_bot + sd_zoff;
    // 原16×3.5卡片通道和45度上肩保留；盖平放后的桥接方向见sd_cap_facade。
    translate([sd_cx,box_y1+0.1,z_sd]) rotate([90,0,0]) linear_extrude(wall_t+1.2)
        polygon([[-sd_w/2,-sd_h/2],[sd_w/2,-sd_h/2],[sd_w/2,sd_h/2],
                 [sd_w/2-4,sd_h/2+4],[-sd_w/2+4,sd_h/2+4],[-sd_w/2,sd_h/2]]);
    slot_cut_y(sd_cx, z_sd, sd_rec[0], sd_rec[1],
               box_y1 - rec_depth, rec_depth + 1);


}

/* ---------- 底面：条栅镂空（减去）与外凸脚（叠加）---------- */
module floor_slats() {
    if (lighten) {
        for (x = slat_xs)
            // 浅仓及承托边挡住的槽整条取消，不保留两端短截；移动喇叭时同样处理。
            if (!(speaker_enabled &&
                  abs(x-speaker_xy[0]) < slat_w/2+speaker_size[0]/2+speaker_fit+1.2 &&
                  slat_y1 > speaker_xy[1]-speaker_size[1]/2-speaker_fit-1.2 &&
                  slat_y0 < speaker_xy[1]+speaker_size[1]/2+speaker_fit+1.2))
                translate([x - slat_w / 2, slat_y0, -0.01])
                    cube([slat_w, slat_y1 - slat_y0, floor_t + 0.02]);
    }
}
module speaker_outline(margin=0) {
    // 喇叭外框四角为直角，浅仓也用直角，避免圆弧吃掉尖角装配余量。
    translate([speaker_xy[0]-speaker_size[0]/2-speaker_fit-margin,
               speaker_xy[1]-speaker_size[1]/2-speaker_fit-margin])
        square([speaker_size[0]+2*(speaker_fit+margin),speaker_size[1]+2*(speaker_fit+margin)]);
}
module speaker_cutouts() {
    if(speaker_enabled) {
        translate([0,0,floor_t-speaker_recess]) linear_extrude(speaker_recess+.02) speaker_outline();
        nx=ceil(speaker_diaphragm[0]/(2*speaker_hole_pitch));
        ny=ceil(speaker_diaphragm[1]/(2*speaker_hole_pitch));
        for(i=[-nx:nx]) for(j=[-ny:ny])
            // 未给振膜圆角半径，按最大圆角保守内收，整个孔都落在11×17包络内。
            if(slot_distance(i*speaker_hole_pitch,j*speaker_hole_pitch,0,0,
                             speaker_diaphragm[0],speaker_diaphragm[1],min(speaker_diaphragm)/2)<=-speaker_hole_d/2)
                translate([speaker_xy[0]+i*speaker_hole_pitch,speaker_xy[1]+j*speaker_hole_pitch,-.01])
                    cylinder(d=speaker_hole_d,h=floor_t+.02,$fn=24);
        x0=speaker_xy[0]+speaker_size[0]/2;
        translate([x0,speaker_xy[1]+speaker_wire_offset-speaker_wire_w/2,floor_t-speaker_recess])
            cube([cav_x1-.6-x0,speaker_wire_w,speaker_recess+.02]);
    }
}
module speaker_hardware() {
    if(speaker_enabled) translate([speaker_xy[0]-speaker_size[0]/2,speaker_xy[1]-speaker_size[1]/2,speaker_z])
        cube(speaker_size); // 仅完整外廓；振膜及焊片形状未实测，不作为打印件
}
module feet() {
    if (foot_h > 0) for (p = [[box_x0 + foot_inset, box_y0 + foot_inset],
              [box_x1 - foot_inset, box_y0 + foot_inset],
              [box_x0 + foot_inset, box_y1 - foot_inset],
              [box_x1 - foot_inset, box_y1 - foot_inset]])
        translate([p[0], p[1], -foot_h])
            cylinder(d1 = foot_d, d2 = foot_d + 2, h = foot_h + 0.01);
}

/* ---------- 开孔标识：FDM 凹印小字，可关闭 ---------- */
labels_enabled = true;
emb_h    = 0.8;       // 实际凹深；1.8mm侧墙仍保留1.0mm背厚
emb_sz   = 2.5;
emb_cut  = 0.1;       // 起挖面外移量（保证挖穿，配合 difference）
emb_font = "Liberation Sans:style=Bold";
assert(!labels_enabled || (emb_h>0 && emb_h<=wall_t-1), "凹字须保留至少1mm侧墙背厚");
lbl_z    = 13.0;      // 统一基线高度（字缩小后上移贴近开孔；下缘留 0.6+）
usb_lbl_z = side_lbl_z;
sd_lbl_z = z_scr_bot+sd_zoff+sd_h/2+4+emb_sz/2+0.3;
// X+ 壁的 PWR 字不能用 lbl_z：SW3 外凹槽（中心 15.95、φ8）下缘 11.95，
// 13.0 的字会挖进槽里。所以 SW3 单独用 sw3_lbl_z，并在这里护住。
assert(!labels_enabled || sw3_lbl_z + emb_sz / 2 < sw3_z - sw3_rec_d / 2 - 0.3,
       "PWR 凹字会压进 SW3 外凹槽：下调 sw3_lbl_z");
assert(!labels_enabled || usb_lbl_z + emb_sz / 2 < j4_z - j4_h / 2 - 0.3,
       "USB 凹字会压进 J4 包胶开口：下调 usb_lbl_z");
assert(!labels_enabled || sd_lbl_z + emb_sz/2 < box_h - 0.5,
       "SD 凹字超出口沿：缩小字或调整 SD 开口");
// 仅ADS-B/AIR调整孔边间距；GPS、USB/PWR保留原字号和位置。
emb_sma_gap = 1.2;
sma_label_spacing = 1.1;
function sma_label_metrics(txt)=textmetrics(txt,size=emb_sz,font=emb_font,spacing=sma_label_spacing);
if(labels_enabled) for(i=[0:1]) {
    left=sma_ports[i][0]-sma_d/2-emb_sma_gap-sma_label_metrics(lbl_sma[i]).size.x;
    limit=i==0?patch_cx+gps_patch_size[0]/2+gps_fit:sma_ports[i-1][0]+sma_d/2;
    assert(emb_sma_gap>=.6 && left>limit+.6,"SMA凹字侵入贴片薄壁或邻孔：缩小字号/字距");
}
// 顶盖字脚朝屏幕Z+；局部字的Y+指向世界Z-。
module wall_text(txt, u, z, face) {
    // 从墙内 emb_h 起，向外多穿 emb_cut，实际凹深恰好为 emb_h。
    _d = emb_h + emb_cut;
    _back = emb_h;
    if (face == 0)      // Y+ 壁：外法线 +y，起点退到 box_y1-_back
        translate([u, box_y1 - _back, z]) rotate([-90, 0, 0])
            linear_extrude(_d)
                text(txt, size = emb_sz, font = emb_font,
                     halign = "center", valign = "center");
    else if (face == 1) // X+ 壁：外法线 +x，起点退到 box_x1-_back
        translate([box_x1 - _back, u, z]) rotate([90, 0, 90])
            linear_extrude(_d)
                text(txt, size = emb_sz, font = emb_font,
                     halign = "center", valign = "center");
    else                // 新 Y+ 顶部舱朝天面
        translate([u, top_y1 - _back, z]) rotate([-90, 0, 0])
            linear_extrude(_d)
                text(txt, size = emb_sz, font = emb_font,
                     halign = "center", valign = "center");
}
module labels() {
    if (len(btns) > 0) wall_text(lbl_pwr, btn_power_x, lbl_z, 0);
    wall_text(lbl_usb, h1_y,        usb_lbl_z, 1);
    wall_text(lbl_pwr, sw3_y,       sw3_lbl_z, 1);   // SW3 在 X+ 壁，字在凹槽下方

}

/* ---------- 按键导向套（内壁，加长导向防倾斜）---------- */
module btn_guides() {
    z_btn = z_scr_bot + btn_zoff;
    for (bx = btns)
        translate([bx, cav_y1 - btn_guide, z_btn]) rotate([-90, 0, 0])
            cylinder(d = btn_boss_d, h = btn_guide + 0.01);
    // 短环内端x=105.25；内侧开口装入销，法兰嵌在墙内沉腔，外肩挡向外脱出。
    translate([cav_x1 - sw3_guide, sw3_y, sw3_z]) rotate([0, 90, 0])
        cylinder(d = sw3_boss_d, h = sw3_guide + 0.01);
}

/* ---------- 按键按压销（法兰止位于导向套内沉腔）----------
   参数化：两面壁的跨距不同（Y+ 3.300 / X+ 4.468），销子长度也不同，
   不能共用一个。默认出 SW3 那款——V4.11仅保留X+电源键。
   ⚠️ 把 RESET/BOOT 填回 btns 时，必须另排一批 plunger(pl_nose, btn_bore_in,
   pl_proud) 的销子，直接拿 SW3 销去装会预压按键。 */
module plunger(nose = sw3_nose, bore_in = sw3_bore_in, proud = sw3_proud) {
    // 自下而上：顶鼻(按压面) → 法兰 → 导向杆
    out_len = wall_t + bore_in + proud;
    fine_l=min(sw3_nose_fine_l,nose);
    cylinder(d=sw3_nose_d,h=fine_l+.01);
    translate([0,0,fine_l]) cylinder(d=pl_shaft_d,h=nose-fine_l+pl_flange_t+out_len);
    translate([0, 0, nose]) cylinder(d = pl_flange_d, h = pl_flange_t);
}

/* ---------- 总装 ---------- */
module main_box() {
    difference() {
        union() {
            box_shell();
            base_extension();
            glass_tabs();
            bosses();
            btn_guides();
            feet();
        }
        cap_fastener_base_cuts();
        boss_bores();
        wall_cutouts();
        floor_slats();
        speaker_cutouts();
        if (labels_enabled) labels();
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
/* ---------- 独立打印：不在盒内悬挂销或转接板 ---------- */
module print_plate() { main_box(); }
module plunger_plate() {
    assert(n_plungers >= 1 && floor(n_plungers) == n_plungers, "n_plungers 必须为正整数");
    for (i = [0:n_plungers - 1]) translate([i * 12, 0, sw3_nose+pl_flange_t+wall_t+sw3_bore_in+sw3_proud]) rotate([180,0,0]) plunger();
}

/* ---------- 装配/剖面：仅为包络，不是打印模型 ---------- */
module hardware_preview() {
    for(x=cap_fix_xs) {
        color([0.65,0.65,0.68]) translate([x,cap_fix_y,cap_head_h]) cylinder(d=2.5,h=10);
        color([0.65,0.65,0.68]) translate([x,cap_fix_y,0.1]) cylinder(d=5.5,h=cap_head_h-0.1);
        color([0.7,0.7,0.72]) translate([x,cap_fix_y,cap_nut_z+.1]) cylinder(d=5/cos(30),h=2,$fn=6);
    }
    for(p=sma_ports) {
        color([0.75,0.6,0.15,0.65]) {
            translate([p[0],top_inner_y1-sma_tail_l,p[1]]) rotate([-90,0,0]) cylinder(d=sma_cb_d,h=sma_tail_l-sma_nut_depth);
            translate([p[0],top_inner_y1-sma_nut_depth,p[1]]) rotate([-90,0,0]) cylinder(d=sma_nut_af/cos(30),h=sma_nut_depth,$fn=6);
        }
        color([0.3,0.3,0.3,0.3]) translate([p[0],top_y1,p[1]]) rotate([-90,0,0]) cylinder(d=p[2],h=sma_external_l);
    }
    gps_hardware();
}
// 逐实体裁切后才着色，避免整体 intersection 把剖面压成单色。
module gps_preview_clip(section=false) {
    if(section) render() intersection() {
        children();
        // 一毫米中央薄剖面，避免远侧仓壁投影覆盖真实空气层。
        translate([gps_cx-0.5,box_y1-1,-1])
            cube([1,top_depth+35,gps_bay_h+3]);
    }
    else children();
}
module gps_hardware(section=false) {
    if(gps_mode=="patch") color([0.85,0.3,0.3]) gps_preview_clip(section) translate([patch_cx-gps_patch_size[0]/2,gps_patch_back_y,gps_patch_z-gps_patch_size[1]/2]) cube([gps_patch_size[0],gps_patch_size[2],gps_patch_size[1]]);
    if(gps_mode=="ceramic") {
        color([0.12,0.45,0.18]) gps_preview_clip(section) translate([gps_cx-gps_board_size[0]/2,gps_pcb_back_y,gps_cz-gps_board_size[1]/2]) cube([gps_board_size[0],gps_board_t,gps_board_size[1]]);
        color([0.9,0.75,0.45]) gps_preview_clip(section) translate([gps_cx+gps_ceramic_offset[0]-gps_ceramic_size[0]/2,gps_pcb_back_y+gps_board_t,gps_cz+gps_ceramic_offset[1]-gps_ceramic_size[1]/2]) cube([gps_ceramic_size[0],gps_ceramic_t,gps_ceramic_size[1]]);
        color([0.6,0.15,0.55,section?1:0.6]) gps_preview_clip(section) translate([gps_cx+gps_back_component_offset[0]-gps_back_component_size[0]/2,gps_pcb_back_y-gps_back_component_size[2],gps_cz+gps_back_component_offset[1]-gps_back_component_size[1]/2]) cube([gps_back_component_size[0],gps_back_component_size[2],gps_back_component_size[1]]);
        color([0.1,0.8,0.9,section?1:0.7]) gps_preview_clip(section)
            gps_corner_contacts(gps_pcb_back_y-gps_glue_t,gps_glue_t);
    }
}
module assembly_preview(exploded=false) {
    color([0.7,0.76,0.82]) render() main_box();
    translate([0,exploded?explode_distance:0,0]) {
        color([0.1,0.5,0.7]) render() cap_body();
        if(gps_mode=="ceramic") translate([0,0,exploded?32:0]) color([0.95,0.5,0.12]) render() gps_tray_body();
        if(preview_hardware && !exploded) { %hardware_preview(); %color([1,0.2,0.2]) coax_envelope(); }
    }
    if(preview_hardware) %union() {
    color([.8,.45,.15,.6]) speaker_hardware();
    color([0.15,0.55,0.2,0.25]) translate([exp_x(0),exp_y(exp_w),z_exp_bot]) cube([exp_l,exp_w,exp_t]);
    color([0.15,0.3,0.6,0.25]) translate([0,0,z_scr_bot]) cube([scr_pcb_l,scr_pcb_w,scr_pcb_t]);
    color([1,0.6,0,0.3]) translate([exp_x(0),52.488,z_exp_top+0.01]) cube([exp_l,8.435,0.15]);
    color([0.1,0.9,0.3,0.3]) translate([sd_cx-sd_w/2,box_y1,z_scr_bot+sd_zoff-sd_h/2]) cube([sd_w,top_depth+30,sd_h]);
    }
}
module cap_print() {
    translate([-box_x0,-floor_t,top_y1]) rotate([-90,0,0]) cap_body();
}
module gps_tray_print() { translate([-gps_tray_x0,gps_tray_z0+gps_tray_h,-gps_tray_back_y]) rotate([90,0,0]) gps_tray_body(); }
ozero = align_origin ? [-box_x0,-box_y0,foot_h] : [0,0,0];
module print_layout() {
    cap_row = top_y1-box_y0+layout_gap;
    small_row = cap_row+max(box_h,gps_bay_h,cap_high_h)-floor_t+layout_gap;
    small_w = (gps_mode=="ceramic"?gps_tray_w+layout_gap:0)+pl_flange_d+12*(n_plungers-1);
    assert(max(top_l,small_w)<=180 && small_row+max(gps_mode=="ceramic"?gps_tray_h:0,pl_flange_d)<=180,
           "打印排版超过A1 mini 180mm平台，请减少按键销数量或调整尺寸");
    translate([-box_x0,-box_y0,foot_h]) main_box();
    translate([0,cap_row,0]) cap_print();
    if(gps_mode=="ceramic") translate([0,small_row,0]) gps_tray_print();
    translate([gps_mode=="ceramic"?gps_tray_w+layout_gap+pl_flange_d/2:pl_flange_d/2,
               small_row+pl_flange_d/2,0]) plunger_plate();
}
if(part=="layout") print_layout();
if(part=="box" || part=="print") translate(ozero) main_box();
if(part=="cap") cap_print();
if(part=="gps_tray") gps_tray_print();
if(part=="assembly") assembly_preview();
if(part=="exploded") assembly_preview(true);
if(part=="gps_section") {
    color([0.7,0.76,0.82]) gps_preview_clip(true) main_box();
    color([0.1,0.5,0.7]) gps_preview_clip(true) cap_body();
    if(gps_mode=="ceramic") color([0.95,0.5,0.12]) gps_preview_clip(true) gps_tray_body();
    gps_hardware(true); // 显式选择GPS剖面时展示模块分层
}
if(part=="section") intersection() {
    main_box(); translate([box_x0,box_y0,0]) cube([box_l/2,top_y1-box_y0,box_h+1]);
}
if(part=="plungers") plunger_plate();
if(part=="plate") mount_plate_body();
