// =============================================================
// Pilot Kit Box — 4.3" 触摸屏版 3D 打印底座（盒体）v4.11 / FDM
// =============================================================
// 结构：单件薄壁盒体，全部自顶部装配（无底盖）。
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
//   - 内腔按【玻璃轮廓】而非 PCB 轮廓，四周到板边 ≥3.8mm，
//     SD 卡座 / SMA 线柱等板边突出物不会顶墙
//   - 玻璃仅靠周圈薄支撑舌托住（螺丝已锁死整叠，只需扣住）
//   - 底板条栅镂空，侧壁 1.8；开孔处仅外沉 0.5、余厚 1.3
//
// 打印提示：默认平底，盒体、按键销、转接板分别导出。
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
part = "box";         // "box" 盒体 / "print" 同盒体（兼容旧入口）
                      // "plungers" 独立按键销 / "plate" 独立转接板 / "section" X 剖面
                      // "assembly" 仅装配预览：含 PCB / IFA / 天线及金属包络
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
lbl_sma  = ["GPS", "ADS-B", "AIR"]; // 对应 sma_x 的三个面板孔

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
// 跨距 3.3mm 专用。X+ 壁的 SW3 跨 4.318mm，**另起一套 sw3_* 参数**，
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
 *      X+ 壁 = cav_x1 − sw3_key_x = 106.3 − 101.982 = 4.318   （多 1.018）
 *    多出来的 1mm 分给导向孔和顶鼻，**优先给导向孔**——导向越长销子越不容易歪，
 *    顶鼻越长越细弱。所以 sw3_bore_in 比 btn_bore_in 多 0.65、sw3_nose 只多 0.2。
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
sw3_btn_ctr_z = 0.95;              // 按钮中心离板面的高度（TS24CA 图纸 1.9/2）
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
sw3_guide   = 4.0;                 // 导向套自内壁面伸入长度（须容下沉腔+法兰+行程）
sw3_bore_in = 1.6;                 // 其中做导向孔的前段 → 导向总长 wall_t+1.6 = 3.4
sw3_nose    = 1.2;                 // 顶鼻
sw3_proud   = 1.6;                 // 静止时销尾高出外壁量（= 可用行程）
side_lbl_z  = 9.4;                 // PWR / USB 共用文字中心高度
sw3_lbl_z   = side_lbl_z;

/* ---------- SD 卡槽（Y+ 壁）---------- */
sd_cx    = glass_left + 59;    // 50.8。实测：卡座中心=玻璃左+59
sd_w     = 16.0;  sd_h = 3.5;
sd_glass_d = 9.35;             // 量尺 z=19.80
sd_zoff  = glass_t + scr_glass_gap + scr_pcb_t - sd_glass_d;      // -1.85
sd_rec   = [22, 8];            // 外侧圆角沉槽（深 rec_depth）
sd_notch = 10;                 // 指甲槽宽（圆角，贯穿余壁）

/* ---------- 顶部（Y+）接口舱；靠背板，避开 SD 操作高度 ---------- */
top_depth = 17;
top_h = 14;
top_roof_t = 1.4;    // 内顶 12.6，给 φ9.5 螺母上下各留 0.35
sma_x = [58, 78, 98];
sma_z = 7.5;
sma_d = 6.6;
sma_cb_d = 9.5;       // 螺母/垫片包络直径，按实物校准
sma_cb_dp = 0;        // 默认平面承压，不削薄 FDM 穿墙面
sma_rec_d = 9.5; sma_rec_dp = 0;
sma_tail_l = 12;      // 从内壁向内的金属尾柱/尾线接头长度，按实物校准
sma_external_d = 12;  // 外部直杆天线根部包络；不代表任意弯头朝向
sma_external_l = 30;  // 预览外部天线根部长度
sma_lbl_z = 1.8;

/* ---------- 壳内 GPS：朝天面在 Y+，天线平面为 XZ ---------- */
gps_mode = "ceramic";   // "patch" 寒武纪薄壁 / "ceramic" 陶瓷凸仓 / "none"
gps_cx = 17;
gps_patch_size = [40, 6, 2.3];
gps_patch_z = 7.5;
gps_wall_t = 0.8;
gps_fit = 0.6;        // 天线与仓侧壁的单边装配间隙
gps_ceramic_size = 20;
gps_ceramic_h = 6.6;  // 总高；2 mm 陶瓷款设 4.6，4 mm 款设 6.6
gps_ceramic_cap_t = 1.2;
gps_clearance = 10;   // 到板框/面板 SMA 金属的机械预留；不是 RF 认证
// 薄片贴薄壁内面；陶瓷靠仓底和侧壁定位，以非金属胶仅固定允许粘贴的边缘。
// 两类天线背面在同一 Y 平面，陶瓷凸出量随总高变化；线缆从内侧敞口引出。

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

top_y0 = box_y1 - corner_r - 0.2; // 方形根部跨过原盒圆角，避免相切夹缝
top_y1 = box_y1 + top_depth;
top_inner_y1 = top_y1 - wall_t;
gps_back_y = top_y1 - gps_wall_t - gps_patch_size[2];
gps_ceramic_face_y = gps_back_y + gps_ceramic_h;
gps_ceramic_y1 = gps_ceramic_face_y + gps_ceramic_cap_t;
gps_ceramic_inner_w = gps_ceramic_size + 2 * gps_fit;
gps_ceramic_z = floor_t + gps_ceramic_inner_w / 2;
gps_ceramic_top = floor_t + gps_ceramic_inner_w + wall_t;
print_y1 = gps_mode == "ceramic" ? max(top_y1, gps_ceramic_y1) : top_y1;

z_glass_b = glass_seat_z;
z_scr_bot = z_glass_b - glass_to_screen_back;
z_exp_top = z_scr_bot - stack_h + exp_support_adjust;
z_exp_bot = z_exp_top - exp_t;
bat_h = z_exp_bot - floor_t;             // 派生柱高/电池仓高，不能再独立调整
board_gap = z_scr_bot - z_exp_top;
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
sw3_span   = cav_x1 - sw3_key_x;                                   // 跨距 4.318
sw3_tip_x  = cav_x1 - sw3_bore_in - pl_flange_t - sw3_nose;        // 顶鼻静止位
sw3_gap    = sw3_tip_x - sw3_key_x;
sw3_gap_wc = sw3_tip_x - (sw3_key_x + sw3_fit);   // 板偏向壁：最易预压
sw3_gap_bc = sw3_tip_x - (sw3_key_x - sw3_fit);   // 板偏离壁：最费行程
sw3_need   = sw3_gap_bc + sw3_travel + 0.15;      // 走完空隙 + 压到底 + 余量
sw3_exp_x1 = exp_x(exp_l);                        // 扩展板 X+ 板边 = 101.25
sw3_seat_face = exp_x(sw3_pcb[0]) + sw3_seat_x;   // 座子前表面 = 100.732
echo(str("SW3: 孔中心 y=", sw3_y, " z=", sw3_z, "  跨距 ", sw3_span,
         "  导向长 ", wall_t + sw3_bore_in, "  静止间隙 ", sw3_gap,
         "（最坏 ", sw3_gap_wc, "）  需行程 ", sw3_need, " / 可用 ", sw3_proud,
         "  销总长 ", sw3_nose + pl_flange_t + wall_t + sw3_bore_in + sw3_proud));
assert(sw3_gap_wc > 0.1,
       "扩展板偏向 USB 壁时销子会常压 SW3（/QON 拉低→反复重启）：减小 sw3_nose");
assert(sw3_need < sw3_proud, "SW3 行程不够：加大 sw3_proud");
assert(sw3_guide - sw3_bore_in >= pl_flange_t + sw3_need,
       "套内沉腔容不下法兰+行程：加大 sw3_guide");
assert(cav_x1 - sw3_bore_in - pl_flange_t - sw3_need > sw3_exp_x1 + 0.3,
       "按到底时法兰会撞扩展板边缘：加大 sw3_nose");
assert(sw3_key_x - sw3_travel > sw3_seat_face + 0.3,
       "顶鼻会压到 TS24CA 座子外壳/固定脚牛角而不是按钮");
assert(cav_x1 - sw3_guide > sw3_exp_x1 + 0.3, "导向套会撞扩展板边缘：缩短 sw3_guide");
assert(sw3_z + sw3_boss_d / 2 < z_scr_bot - 0.3, "导向套顶会撞屏板底面：SW3 太高");
assert(abs(sw3_y - h1_y) > (sw3_rec_d + usb_rec[0]) / 2 + 0.5,
       "SW3 外凹与 H1 USB 沉槽在 X+ 壁上重叠");
// 圆角矩形有符号距离：校准接口高度/尺寸时，检查穿孔仍给 LED 留足实体。
function slot_distance(y, z, cy, cz, w, h) =
    let(r = min(slot_r, min(h, w)/2 - 0.01),
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

/* ---------- 顶部装配空间自检 ---------- */
assert(gps_mode == "patch" || gps_mode == "ceramic" || gps_mode == "none", "未知 gps_mode");
assert(gps_ceramic_h > 0 && gps_ceramic_size > 0 && gps_fit > 0, "GPS 尺寸必须为正");
assert(min(gps_patch_size) > 0 && gps_ceramic_cap_t >= 0.6 && gps_clearance >= 10,
       "GPS 天线尺寸/壁厚无效，或机械预留小于 10 mm");
assert(gps_wall_t >= 0.6 && gps_wall_t <= wall_t, "薄壁厚度须在 0.6 与 wall_t 之间");
assert(top_roof_t >= 1.0 && top_h > floor_t + top_roof_t, "顶部舱壁厚/高度无效");
assert(len(sma_x) == 3 && len(lbl_sma) == 3, "需要 GPS、ADS-B、AIR 三个 SMA");
assert(sma_d > 0 && sma_cb_d >= sma_d && sma_external_d > 0 && sma_external_l > 0,
       "SMA 孔径/金属/外部天线包络无效");
assert(sma_tail_l > 0 && top_inner_y1 - sma_tail_l > exp_y(0) + 0.5,
       "SMA 尾柱进入扩展板边界：加深顶部舱或缩短实际接头");
assert(sma_z - sma_cb_d / 2 >= floor_t + 0.25 &&
       sma_z + sma_cb_d / 2 <= top_h - top_roof_t - 0.25, "SMA 螺母碰底板/顶棚");
assert(wall_t - sma_cb_dp - sma_rec_dp >= 1.2, "SMA 穿墙承压面太薄");
assert(sma_z + sma_external_d / 2 < z_scr_bot + sd_zoff - sd_h / 2 - 1,
       "SMA 外部直杆包络遮挡 SD 通道；减小包络或降低接口");
for (i = [0:len(sma_x)-1]) {
    assert(sma_x[i] - sma_cb_d / 2 > cav_x0 && sma_x[i] + sma_cb_d / 2 < cav_x1,
           "SMA 螺母碰舱侧壁");
    if (i > 0) assert(sma_x[i] - sma_x[i-1] > max(sma_cb_d, sma_external_d) + 1,
                     "SMA 间距不足或顺序错误");
}
if (gps_mode != "none") {
    gps_width = gps_mode == "patch" ? gps_patch_size[0] : gps_ceramic_size;
    assert(gps_back_y - exp_y(0) >= gps_clearance, "GPS 与扩展板边框预留不足");
    assert(min(sma_x) - sma_cb_d/2 - gps_cx - gps_width/2 >= gps_clearance,
           "GPS 与 SMA 金属的预留不足");
}
if (gps_mode == "patch") {
    assert(gps_cx - gps_patch_size[0]/2 - gps_fit > cav_x0 &&
           gps_cx + gps_patch_size[0]/2 + gps_fit < cav_x1, "贴片薄壁区越过侧壁");
    assert(gps_patch_z - gps_patch_size[1]/2 - gps_fit > floor_t &&
           gps_patch_z + gps_patch_size[1]/2 + gps_fit < top_h - top_roof_t,
           "贴片薄壁区越过底板/顶棚");
}
if (gps_mode == "ceramic") {
    assert(gps_cx - gps_ceramic_inner_w/2 - wall_t > box_x0 &&
           gps_cx + gps_ceramic_inner_w/2 + wall_t < sd_cx - sd_rec[0]/2 - 1,
           "陶瓷凸仓越界或遮挡 SD 操作区");
    assert(gps_ceramic_top < z_glass_b - tab_t, "陶瓷凸仓与玻璃托舌相交");
    for (bx = btns) assert(abs(bx - gps_cx) > gps_ceramic_inner_w/2 + wall_t + btn_boss_d/2,
                          "可选屏板按键与陶瓷仓相交");
}

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

/* ---------- 顶部接口舱：内口与主腔连通，顶棚打印需要桥接/支撑 ---------- */
module top_bay_outer() {
    translate([box_x0, top_y0, 0]) linear_extrude(top_h)
        hull() {
            square([box_l, 0.1]);
            for (x = [corner_r, box_l-corner_r])
                translate([x, top_y1-top_y0-corner_r]) circle(r=corner_r);
        }
    if (gps_mode == "ceramic")
        translate([gps_cx - gps_ceramic_inner_w/2 - wall_t, top_y0, 0])
            cube([gps_ceramic_inner_w + 2*wall_t,
                  gps_ceramic_y1 - top_y0, gps_ceramic_top]);
}
module top_bay_cavity() {
    translate([cav_x0, top_y0 - 0.1, floor_t])
        cube([cav_x1-cav_x0, top_inner_y1-top_y0+0.1, top_h-top_roof_t-floor_t]);
    if (gps_mode == "patch")
        translate([gps_cx-gps_patch_size[0]/2-gps_fit, top_inner_y1-0.01,
                   gps_patch_z-gps_patch_size[1]/2-gps_fit])
            cube([gps_patch_size[0]+2*gps_fit, wall_t-gps_wall_t+0.01,
                  gps_patch_size[1]+2*gps_fit]);
    if (gps_mode == "ceramic")
        translate([gps_cx-gps_ceramic_inner_w/2, top_y0-0.1, floor_t])
            cube([gps_ceramic_inner_w, gps_ceramic_face_y-top_y0+0.1, gps_ceramic_inner_w]);
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

    /* --- SW3 主电源键（X+）：长导向孔 + 套内法兰沉腔 + 外侧圆凹 ---
       与上面 POWER 一节同构，只是切割方向从 +Y 换成 +X、参数换成 sw3_*。 */
    slot_cut_x(sw3_y, sw3_z, sw3_d, sw3_d, cav_x1 - sw3_bore_in, cut_len);
    translate([cav_x1 - sw3_guide - 0.5, sw3_y, sw3_z]) rotate([0, 90, 0])
        cylinder(d = btn_pkt_d, h = sw3_guide - sw3_bore_in + 0.5);
    translate([box_x1 - rec_depth, sw3_y, sw3_z]) rotate([0, 90, 0])
        cylinder(d = sw3_rec_d, h = rec_depth + 0.01);

    /* --- SD 卡槽（Y+）：穿墙槽 + 外侧圆角浅沉槽 + 圆角指甲槽 --- */
    z_sd = z_scr_bot + sd_zoff;
    slot_cut_y(sd_cx, z_sd, sd_w, sd_h, cav_y1 - 1, cut_len);
    slot_cut_y(sd_cx, z_sd, sd_rec[0], sd_rec[1],
               box_y1 - rec_depth, rec_depth + 1);

    /* --- SMA ×3：顶部舱朝天面，平面承压 --- */
    for (sx = sma_x) {
        translate([sx, top_inner_y1-0.01, sma_z]) rotate([-90, 0, 0])
            cylinder(d = sma_d, h = wall_t+0.02);
        if (sma_cb_dp > 0)
            translate([sx, top_inner_y1-0.01, sma_z]) rotate([-90, 0, 0])
                cylinder(d = sma_cb_d, h = sma_cb_dp+0.01);
        if (sma_rec_dp > 0)
            translate([sx, top_y1-sma_rec_dp, sma_z]) rotate([-90, 0, 0])
                cylinder(d = sma_rec_d, h = sma_rec_dp+0.01);
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
    if (foot_h > 0) for (p = [[box_x0 + foot_inset, box_y0 + foot_inset],
              [box_x1 - foot_inset, box_y0 + foot_inset],
              [box_x0 + foot_inset, box_y1 - foot_inset],
              [box_x1 - foot_inset, box_y1 - foot_inset]])
        translate([p[0], p[1], -foot_h])
            cylinder(d1 = foot_d, d2 = foot_d + 2, h = foot_h + 0.01);
}

/* ---------- 开孔标识：FDM 浅凹小字，可关闭 ---------- */
labels_enabled = true;
emb_h    = 0.35;
emb_sz   = 2.5;
emb_cut  = 0.1;       // 起挖面外移量（保证挖穿，配合 difference）
emb_font = "Liberation Sans:style=Bold";
lbl_z    = 13.0;      // 统一基线高度（字缩小后上移贴近开孔；下缘留 0.6+）
usb_lbl_z = side_lbl_z;
sd_lbl_z = z_scr_bot + sd_zoff + sd_rec[1]/2 + emb_sz/2 + 0.5;
// X+ 壁的 PWR 字不能用 lbl_z：SW3 外凹槽（中心 15.95、φ8）下缘 11.95，
// 13.0 的字会挖进槽里。所以 SW3 单独用 sw3_lbl_z，并在这里护住。
assert(!labels_enabled || sw3_lbl_z + emb_sz / 2 < sw3_z - sw3_rec_d / 2 - 0.3,
       "PWR 凹字会压进 SW3 外凹槽：下调 sw3_lbl_z");
assert(!labels_enabled || usb_lbl_z + emb_sz / 2 < j4_z - j4_h / 2 - 0.3,
       "USB 凹字会压进 J4 包胶开口：下调 usb_lbl_z");
assert(!labels_enabled || sd_lbl_z + emb_sz/2 < box_h - 0.5,
       "SD 凹字超出口沿：缩小字或调整 SD 开口");
module wall_text(txt, u, z, face) {
    // 从墙内 emb_h 起，向外多穿 emb_cut，实际凹深恰好为 emb_h。
    _d = emb_h + emb_cut;
    _back = emb_h;
    if (face == 0)      // Y+ 壁：外法线 +y，起点退到 box_y1-_back
        translate([u, box_y1 - _back, z]) rotate([-90, 0, 0]) rotate([0, 0, 180])
            linear_extrude(_d)
                text(txt, size = emb_sz, font = emb_font,
                     halign = "center", valign = "center");
    else if (face == 1) // X+ 壁：外法线 +x，起点退到 box_x1-_back
        translate([box_x1 - _back, u, z]) rotate([90, 0, 90])
            linear_extrude(_d)
                text(txt, size = emb_sz, font = emb_font,
                     halign = "center", valign = "center");
    else                // 新 Y+ 顶部舱朝天面
        translate([u, top_y1 - _back, z]) rotate([-90, 0, 0]) rotate([0, 0, 180])
            linear_extrude(_d)
                text(txt, size = emb_sz, font = emb_font,
                     halign = "center", valign = "center");
}
module labels() {
    if (len(btns) > 0) wall_text(lbl_pwr, btn_power_x, lbl_z, 0);
    wall_text(lbl_sd,  sd_cx,       sd_lbl_z, 0);
    wall_text(lbl_usb, h1_y,        usb_lbl_z, 1);
    wall_text(lbl_pwr, sw3_y,       sw3_lbl_z, 1);   // SW3 在 X+ 壁，字在凹槽下方
    for (i = [0:len(sma_x)-1]) wall_text(lbl_sma[i], sma_x[i], sma_lbl_z, 2);
}

/* ---------- 按键导向套（内壁，加长导向防倾斜）---------- */
module btn_guides() {
    z_btn = z_scr_bot + btn_zoff;
    for (bx = btns)
        translate([bx, cav_y1 - btn_guide, z_btn]) rotate([-90, 0, 0])
            cylinder(d = btn_boss_d, h = btn_guide + 0.01);
    // SW3（X+ 壁）：套内端 x=102.3，扩展板板边 101.25，留 1.05 不撞（有断言护着）
    translate([cav_x1 - sw3_guide, sw3_y, sw3_z]) rotate([0, 90, 0])
        cylinder(d = sw3_boss_d, h = sw3_guide + 0.01);
}

/* ---------- 按键按压销（法兰止位于导向套内沉腔）----------
   参数化：两面壁的跨距不同（Y+ 3.300 / X+ 4.318），销子长度也不同，
   不能共用一个。默认出 SW3 那款——V4.7 只有 X+ 壁开孔。
   ⚠️ 把 RESET/BOOT 填回 btns 时，必须另排一批 plunger(pl_nose, btn_bore_in,
   pl_proud) 的销子，直接拿 SW3 销去装会预压按键。 */
module plunger(nose = sw3_nose, bore_in = sw3_bore_in, proud = sw3_proud) {
    // 自下而上：顶鼻(按压面) → 法兰 → 导向杆
    out_len = wall_t + bore_in + proud;
    cylinder(d = pl_shaft_d, h = nose + pl_flange_t + out_len);
    translate([0, 0, nose]) cylinder(d = pl_flange_d, h = pl_flange_t);
}

/* ---------- 总装 ---------- */
module main_box() {
    difference() {
        union() {
            box_shell();
            top_bay_outer();
            glass_tabs();
            bosses();
            btn_guides();
            feet();
        }
        top_bay_cavity();
        boss_bores();
        wall_cutouts();
        floor_slats();
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
    for (i = [0:n_plungers - 1]) translate([i * 12, 0, 0]) plunger();
}

/* ---------- 装配预览（仅 part="assembly"，不可当打印模型）---------- */
module assembly_preview() {
    color([0.75, 0.8, 0.85, 0.35]) main_box();
    color([0.1, 0.5, 0.2, 0.6])
        translate([exp_x(0), exp_y(exp_w), z_exp_bot]) cube([exp_l, exp_w, exp_t]);
    color([0.1, 0.3, 0.6, 0.5])
        translate([0, 0, z_scr_bot]) cube([scr_pcb_l, scr_pcb_w, scr_pcb_t]);
    // ANT1 F.Fab 实际本体。整条橙色带是 PCB 二维禁铜规则，不等于三维 RF 净空。
    color([0.95, 0.6, 0.05, 0.8])
        translate([24.672, 52.488, z_exp_top]) cube([50, 7.5, 0.15]);
    color([0.95, 0.6, 0.05, 0.2])
        translate([exp_x(0), 52.488, z_exp_top+0.16]) cube([exp_l, 8.435, 0.1]);
    for (sx = sma_x) {
        color([0.7, 0.6, 0.2, 0.7])
            translate([sx, top_inner_y1-sma_tail_l, sma_z]) rotate([-90, 0, 0])
                cylinder(d=sma_cb_d, h=sma_tail_l);
        color([0.7, 0.6, 0.2, 0.7])
            translate([sx, top_inner_y1, sma_z]) rotate([-90, 0, 0])
                cylinder(d=sma_d-0.2, h=wall_t);
        color([0.3, 0.3, 0.3, 0.25])
            translate([sx, top_y1, sma_z]) rotate([-90, 0, 0])
                cylinder(d=sma_external_d, h=sma_external_l);
    }
    if (gps_mode == "patch") color([0.8, 0.2, 0.5, 0.8])
        translate([gps_cx-gps_patch_size[0]/2, gps_back_y, gps_patch_z-gps_patch_size[1]/2])
            cube([gps_patch_size[0], gps_patch_size[2], gps_patch_size[1]]);
    if (gps_mode == "ceramic") color([0.9, 0.85, 0.55, 0.8])
        translate([gps_cx-gps_ceramic_size/2, gps_back_y, gps_ceramic_z-gps_ceramic_size/2])
            cube([gps_ceramic_size, gps_ceramic_h, gps_ceramic_size]);
    if (mount_plate) color([0.5, 0.5, 0.5, 0.5]) translate([0,0,-mp_t-foot_h]) mount_plate_body();
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
echo(str("PWR/SW3 孔 y=", AY(sw3_y), "  z=", AZ(sw3_z),
         " (距玻璃面 ", box_h - sw3_z, ")  φ", sw3_d,
         "  外凹φ", sw3_rec_d, "深", rec_depth, "  在 x=", AX(box_x1),
         " 壁（USB 侧）  销凸出 ", sw3_proud));
echo(str("J4 USB: y=", AY(j4_y), " z=", j4_z, "（Z 暂定，需实测）; D8/D9 y=",
         [for (v = led_ys) AY(v)], " z=", led_z, "（侧窗不是导光柱）"));
echo(str("Y+ 壁按键孔: ", btn_on ? str("开 ", btns) : "不开（V4.7 堵掉，见 btns 注释）"));
echo(str("SD 槽     x=", AX(sd_cx), "  z=", AZ(z_scr_bot + sd_zoff),
         " (距玻璃面 ", sd_glass_d, ")  ", sd_w, "x", sd_h,
         "  外框", sd_rec[0], "x", sd_rec[1], " 深", rec_depth));
echo(str("USB 孔    y=", AY(h1_y), "  z=", AZ(z_scr_bot + usb_zoff),
         " (距玻璃面 ", usb_glass_d, ")  ", usb_w, "(y向)x", usb_h,
         "(z向)  外框", usb_rec[0], "x", usb_rec[1], "深", rec_depth,
         "  在 x=", AX(box_x1), " 壁"));
echo(str("顶部 SMA x=", [for (v=sma_x) AX(v)], " y=", AY(top_y1), " z=", sma_z,
         " φ", sma_d, " 尾柱内端y=", AY(top_inner_y1-sma_tail_l)));
echo(str("GPS 模式=", gps_mode, " 背面y=", AY(gps_back_y),
         " 顶部总外形 Y=", print_y1-box_y0, "（不含外部天线）"));
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
    if (part == "plate") mount_plate_body();
    if (part == "box") main_box();
    if (part == "assembly") assembly_preview();
    if (part == "section")
        difference() {
            main_box();
            translate([scr_pcb_l / 2, box_y0 - 1, -1])
                cube([box_l, print_y1 - box_y0 + 2, box_h + 2]);
        }
}
if (part == "plungers") plunger_plate();
