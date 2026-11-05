# OPNA Studio P2 — 设计说明

**v1.0.0 · 2026-11**

> 目标板：**STM32 Primer2**（STM32F103VET6，Cortex-M3，512 kB Flash / 64 kB RAM，
> 128×160 ST7732 LCD，五向摇杆，STW5094A codec，microSD，LIS3LV02 三轴加速度计）。
> 音频核心基于 pocket-fmdsp；本工程是**独立实现**的演奏/编曲固件。

---

## 1. 设计目标

1. **把参考工程的“播放器”变成“乐器”**：不再只是选一首 `.M` 播放，而是能编辑
   pattern、现场弹奏、调混音、看频谱，并且把作品存回 SD。
2. **不牺牲音频正确性**：FM / rhythm 路径沿用参考工程的 bit-exact 整数核心；
   所有新功能围绕它构建，不在每个采样点上加浮点或分支。
3. **把 64 kB RAM 用在声音和图形上**：不引入动态分配、不放大缓冲区；用
   sample-accurate 的事件切片代替高频率 tick 轮询。
4. **UI 必须像一台 2008 年的掌机，而不是网页**：128×160、4×8 字体、五页 tab、
   深蓝/金色/青色配色、像素级图标和 VU。

---

## 2. 声音架构

### 2.1 OPNA 声部分配

| 引擎轨 | OPNA 资源 | 用途 |
|--------|-----------|------|
| `FM1`–`FM6` | FM ch1–6（4 算子/通道） | bass、lead、pad、arp、pluck、sparkle |
| `SSG1`–`SSG2` | SSG A/B 方波 | 高音琶音、副旋律 |
| `NOISE` | SSG C，噪声模式 | hi-hat / 打击点缀 |
| `DRM` | rhythm BD/SD/TOP/HH/TOM/RIM | 鼓组 |

“10 轨”是编曲视图；物理上仍是一颗 YM2608，共 15 个可发声部。FM6 在 SYN 页
被 live layer 独占，其余页归还给音序器。

**默认混音是 3 FM + SSG1 + 鼓**：FM1–3 与 SSG1 开，FM4–6、SSG2、NOISE
静音。这是参考工程验证过的 3-FM 实时预算，并把剩下的固定成本压到最低。
FM4–6 / SSG2 / NOISE 在 MIX 页逐轨打开，6 FM 是这块板子的极限模式。
静音轨通过 `opna_set_mask()` 在 OPNA 核里直接旁路（见 2.6）。

### 2.2 FM 寄存器与 patch

物理 slot 顺序是 **S1, S3, S2, S4**（YM2608 的硬件顺序）。每个 `st_op`：

```
0x30+4*s+ch : DT(7:4) | MUL(3:0)
0x40+...    : TL 0..127
0x50+...    : KS(7:6) | AR(4:0)
0x60+...    : DR 0..31
0x70+...    : SR 0..31
0x80+...    : SL(7:4) | RR(3:0)
0xB0+ch     : FB(5:3) | ALG(2:0)
0xB4+ch     : 0xC0（L+R，关闭 LFO）
0xA4+ch     : BLK(5:3) | FNUM(10:8)
0xA0+ch     : FNUM(7:0)   （先写高字节再写低字节）
0x28        : 0xF0|bank<<2|ch  key-on / 0x00|... key-off
```

载波由算法决定（`st_patch_carriers(alg)`）：

| 算法 | 可发声的物理 slot |
|------|-------------------|
| 0–3 | S4 |
| 4 | S3 + S4 |
| 5–6 | S3 + S2 + S4 |
| 7 | 全部 4 个 |

每次 note-on：

1. 先 key-off（保证重触发）；
2. 写 fnum/blk；
3. 对每个 slot 写 TL：非载波用 patch 的 base TL；载波再加
   `(15-vel)*2 + (15-track_level)*2` 的衰减（0–60）；
4. key-on。

TL 的更新只发生在 note-on / 混音器改电平，不在 per-sample 路径里。

8 个 patch 的 alg/fb 与参数见 [`firmware/studio/patches.c`](firmware/studio/patches.c)：
BASS(alg2, fb3)、LEAD(alg4, fb6)、PAD(alg5, fb4)、PLUCK(alg4, fb5)、
BELL(alg4, fb2)、BRASS(alg2, fb3)、ORGAN(alg7, fb0)、GLASS(alg5, fb3)。

### 2.3 频率表

`firmware/tools/gen_studio_notes.py` 生成 `notes.h`：

```
f_out = (fnum << blk) / 2 * fs / 2^20     (MUL=1)
fnum  = round(f_out * 2^21 / (fs * 2^blk))
```

每个 MIDI note 选使 fnum 落在 1024–2047 的 block。SSG 分频
`TP = round(249600 / (2*f))`，clamp 1..4095。

### 2.4 SSG / 噪声 / 鼓

- SSG A/B：写 12-bit 分频 + 4-bit volume；note-off 写 0。软件在 2.3 ms 的
  子 tick 上做 4-bit 衰减（噪声轨用快速衰减做 hi-hat）。
- Mixer 0x07 = `0x1C`：tone A/B 开、tone C 关、noise C 开。
- 噪声周期 0x06 = `0x18`。
- 鼓：0x11 total level `0x2A`，每声部 0x18+d = `0xC0|level`，0x10 触发。
  力度 0–15 映射到硬件衰减字段。
- **电平配平**：参考核的 SSG voltable 相对 FM 约小 8 倍；本工程把它整体 ×4
  （见 `src/opna/opna_ssg.c` 顶部注释）。FM 与鼓路径未改，仍与参考 bit-exact。

### 2.5 主增益

`studio_render()` 在 `opna_mix()` 之后做一次 `×3`（并 clamp 到 int16）：

- 24 s demo（3-FM 默认）的 sim WAV peak 约 18.4 k、host 约 19.1 k，0 clipped；
  RMS 约 3.2 k；
- 设备端再由 UI 音量 0–10（Q15 0…32767）缩放，默认 8（×0.72）；
- 这样既有耳机/小喇叭的响度，又保留 4 dB 左右余量给密集 pattern。

### 2.6 CPU 预算、mask 与按页分析

三轮上板实测：

| 配置 | CPU |
|------|-----|
| 6 FM（旧版） | **107%** |
| 早期 3 FM | **80–90%** |
| 3 FM，暂停 | **57%** |
| **v1.0：3 FM + SSG1 + 鼓** | **40–50%** |
| 同上，暂停 | **0%** |
| 6 FM + SSG2/NOISE + 鼓 | **≈80%**（能力边界） |

107 → 80–90 说明 FM 通道数确实是主项；但 80–90 仍然高，且暂停还有 57%：
**固定成本（SSG mixer / 鼓 / 每采样后处理）和停止时仍在渲染释放尾音**。
优化分四层：

1. **默认只留 SSG1**：FM4–6、SSG2、NOISE 置 mute。SSG1 是短 gates 的高音
   琶音，大部分时间音量已经归零。
2. **SSG 动态 mask**：`update_mask()` 在音量 0 时置对应 SSG mask；三路都静音
   时 `opna_mix()` 直接跳过整个 SSG mixer。SSG 的 `smpl0/1/2` 也会跳过被
   mask 通道的 closed-form 计算。
3. **暂停/停止停渲染**：`studio_is_idle()`（未播放、无 live/arp/preview、
   无活动音符）为真时，app task 只 `memset` 静音块并写 DMA，不调用
   `studio_render()`；暂停 CPU 从 57% 降到接近 0。
4. **按页分析**：FFT/scope 只在 VIS，VU 只在 MIX；SEQ/SYN/SYS 不做逐采样
   capture。`engine.c` 用 `-O3`，`master_gain()` / `capture()` 标记 `PFM_HOT`
   放进 `.ramfunc`。

另外：

- **旧存档 v1→v2**：旧版存档会把 FM4–6 恢复成开启；加载 v1 时强制
  FM4–6/SSG2/NOISE 静音，pattern 保留，MIX 里再开。
- **LCD tearing**：视频确认是整屏缓慢移动的 tear（GRAM 写入与扫描重叠），
  不是 FSMC 时序/背光 PWM。FSMC 恢复参考的 `ADDSET=2/ADDHLD=2/DATAST=2/
  BUSTURN=5`，让剩余写入窗口尽量短；背光保持 16 kHz。真正的修复是
  **partial/dirty rendering**：`ui_draw()` 只在输入/切页时整页重绘，
  pattern 链走 `ui_redraw_pattern()`，其余帧走 `ui_anim()` 只画
  ruler/status/hint/VU/FFT/tilt 等小区域。默认 SEQ 空闲不再有 25 fps
  全屏写入。TE 引脚在原理图上是 NC，无法硬件同步。

v1.0 实测：**40–50%，暂停 0%**；全部通道约 **80%**。6 FM 是
压力测试模式，CPU 表会冲到 100%+。为了把 80% 时的音频迟滞压下去：

- `studio_render()` 也标记 `PFM_HOT`，与 master gain/capture 一起进 `.ramfunc`；
- LCD task 只有 `board_audio_ring_fill() > 256` 时才画一帧，避免 UI 抢音频；
- 输入只重绘 dirty rect；VIS 频谱进 128×73 SRAM canvas 再 flush；
- 动画不再大块清屏（VU 只画条、SYN 只画旧/新键、倾角只擦 3×3）。

如果 6 FM 仍有 `UND`，优先减到 4/5 FM 或关 VIS 的实时 FFT；6 FM 不是默认。

---

## 3. 采样级音序器

### 3.1 数据模型

```c
struct st_step { uint8_t note, vel, gate; };
struct st_song {
  struct st_step step[4][10][16];  // 1920 B
  uint8_t mute[10], level[10], patch[6];
};
```

- `note=0xFF` 为 rest；DRM 轨的 note 是鼓 id 0–5。
- `gate 0..14` = 步长的 `(gate+1)/16`；`gate=15` = **tie**（不自动止音）。
- pattern 链 A→B→C→D→A，每 16 步一小节推进。

### 3.2 时间

```
step_frames   = 55466*60 / (BPM*4)        // 124 BPM -> 6710 帧
swing_frames  = step_frames * swing / 200 // 12% -> 402 帧
step_start(s) = bar_start + s*step_frames + (s&1 ? swing_frames : 0)
```

- 事件在 **精确的绝对帧** 上触发；`studio_render()` 每次只渲染到“下一个
  step / next note-off / next arp tick”之前：

```c
n = min(frames, next_event - pos, 256);
opna_mix(&opna, buf, n);
pos += n;
```

- 因为寄存器写入发生在两次 `opna_mix` 之间，音符起止是采样精确的；渲染缓冲
  大小（host 512 / sim 256 / HW 256）不影响输出。
- BPM / swing 在播放中改变时，用当前 step 反推 `bar_start` 并重排下一个事件。

### 3.3 note-off 与 tie

- 每个轨记录 `note_active[t]`、`note_off_at[t]`。
- `note_off_at=0` 表示 tie；下次同轨 note-on / rest / stop 时 key-off。
- 鼓不占用 note-off（一次性 PCM）。

### 3.4 live layer（FM6）

- `studio_live_focus(1)` 在进入 SYN 页时调用；音序器跳过 FM6 的 step 触发。
- `studio_live_key(note, vel)`：ARP 关时立即 note-on；ARP 开时在下一个 16 分
  音符触发琶音。
- ARP 音型 `{0,3,7,10,12,10,7,3}`（minor），每 `step_frames` 推进一次。
- LATCH：松开 C 后保持 `live_latched`，ARP 继续。
- Tilt：`dx/500` → ±2 半音；在 live note 上直接改 fnum（不重触发）。
- 离开 SYN：`live_focus(0)` 强制 key-off / 清状态，FM6 归还音序器。

---

## 4. 可视化

### 4.1 FFT

- 256 点、Hann 窗、Q15 twiddle、int32 蝶形、int64 中间乘积（保证不溢出）。
- 捕获：引擎把每个输出帧的 `(L+R)/2` 写入 256 项 mono ring。
- 显示 64 个 bin：`bin(d)=round(127*(d/63)^2)`，低频展开；高频加 `d/16` tilt。
- 幅度 → 高度：

```
q = |re|+|im| >> 7
lg = 31-clz(q)
h = (lg-4)*6 + frac*6/1024 + d/16   // clamp 0..63
```

- 峰值保持：`peak[d]` 只在上冲时更新，每次分析衰减 2；频谱本身下冲时每次
  衰减 2，形成“快起慢落”。
- 颜色是**固定垂直色带**（不是每根柱子各自渐变）：底部绿、越过中线黄、冲顶红。
  这样短柱不会被染红，只有真正高的频段才出现红/黄。

### 4.2 示波器

- 在 256 项 ring 中找上升过零点，取 128 个隔点样本，波形不滚动。
- UI 把相邻列连成 1 px 垂直段，得到连续的轨迹；中心线 + 3 条网格。

### 4.3 电平 / 负载

- 引擎在 `capture()` 里记录每个 256 帧切片的峰值，`vu_hold` 只升不降；
  `studio_analysis_update()` 每帧读出后衰减 1/8。
- VIS 底部显示 `CPU / UND / L / R / FM / SSG / DRM`；CPU 与 DSP 分解在 HW 上
  由 DWT + `pfm_prof_cyc` 实测。

---

## 5. UI

### 5.1 布局（128×160）

```
y   0..7    status: OPNA.P2 | ▶ PLAY | CPU nn%
y   8..8    page accent line
y   9..26   header: 2× page title + 2 lines of right-aligned context
y  27..136  page content
y 137..147  hint bar（4 s 轮播，操作反馈时临时覆盖）
y 148..159  5 tabs: SEQ SYN MIX VIS SYS
```

- 字体：Misaki 8×8，half-width 4×8 / full-width 8×8，scale 1/2/4。
- 颜色（RGB565）：

| 名称 | RGB | 用途 |
|------|-----|------|
| `C_BG` | 8,10,18 | 背景 |
| `C_PANEL` | 20,26,46 | header / 卡片 |
| `C_PANEL2` | 30,38,66 | 选中行 / 分隔 |
| `C_LINE` | 42,52,86 | 网格线 |
| `C_TEXT` | 214,222,243 | 主文字 |
| `C_DIM` / `C_DIMMER` | 126,138,170 / 68,78,106 | 次要 / 禁用 |
| `C_GOLD` | 255,212,94 | SEQ / 标题 / 选中 |
| `C_CYAN` | 99,217,255 | VIS / 数值 |
| `C_GREEN` | 99,242,166 | SYN / 播放 / 正常 |
| `C_RED` | 255,92,110 | 静音 / 错误 / 峰值 |
| `C_ORANGE` | 255,155,69 | MIX |
| `C_PURPLE` | 184,140,255 | SYS |

- 轨道色：FM 金/橙/珊瑚/紫/蓝/薄荷，SSG 青，NOISE 黄绿，DRM 橙红。
- 页面 accent：SEQ 金、SYN 绿、MIX 橙、VIS 青、SYS 紫；tab 选中时用
  `mix(accent, panel, 55/255)` 做底色 + 2 px 顶部色条。

### 5.2 快速文字

原参考的 `gfx.c` 每个像素调一次 `board_lcd_pixel()`，每个像素都重设 LCD
window；在 20 fps 的全页重绘下这是主要瓶颈。本工程改为：

1. 在栈外的静态 `uint16_t g_glyph[32*32]` 中把 glyph 展开成 scale；
2. 调 `board_lcd_blit(x,y,w,h,buf)`；
3. 后端只设 **一次** window，连续写 `w*h` 个像素。

对 4×8 glyph 是 32 次 window → 1 次；对 scale-2 标题是 128 次 → 1 次。

### 5.3 绘制任务

- HW：`app` task（prio 2）负责音频 + 输入；`lcd` task（prio 1）每 40 ms
  画一帧。音频永远抢占 LCD。
- **页面切换** → `ui_draw()` 整页 + `board_lcd_clear()`。
- **输入** → `ui_draw_dirty()`：页面函数把旧/新 cell、row、key、footer、hint
  等加入 dirty rect 列表；BSP 的 `board_lcd_clip_*` 让整页 draw 只写这些
  rect。一次方向键不会重写状态栏/页签/整个网格。
- **pattern 链** → `ui_redraw_pattern()`：只画 header 右侧/pattern 条/ruler/
  网格/footer。
- **其余帧** → `ui_anim()`：状态 CPU 46×8（500 ms）、hint 128×11（3.5 s）、
  SEQ ruler 96×3、MIX VU 条 8×76、SYN tilt 3×3 点 + 旧/新键 + 信息行、
  VIS 内容。动画不做大块 `gx_fill` 清屏。
- **VIS** 的频谱/示波器画进 128×73 SRAM canvas（18 688 B），再一次性 flush；
  面板看不到“先清空再画”的空白帧。
- `ui_draw()` / `ui_anim()` 按页调用分析：VIS → FFT+scope，MIX → VU，
  其余页不分析。
- 跨任务状态分两层：app 改 `U`，`publish()` 写 `V_live`；LCD 每帧在临界区
  把 `V_live` 快照到 `V` 并原子取走 dirty 列表，draw 只读 `V`。
- LCD task 在 `board_audio_ring_fill() > 512` 时才画，音频环不足时宁可掉一帧。

### 5.4 输入状态机

| 常量 | 值 | 含义 |
|------|----|------|
| debounce | 14 ms | 按下沿去抖 |
| settle | 250 ms | 单按/双击判定窗口；也是三连的每击窗口 |
| long | 550 ms | 长按切页 |
| hold | 600 ms | SEQ 的 hold 改 pattern/BPM |
| repeat | 160 ms | hold 重复 |

- 五向是一根摇杆，**取消 C+方向**。C 按下只做 live note（SYN）和计时；
  C 释放累计 `c_taps`，在 250 ms settle 后：
  - 1 击 → `page_tap()`；
  - 2 击 → `page_double()`（播放/停止）；
  - 3 击 → 切换 **MOD**（SEQ/SYN）；MOD 会**一直保持**，方向由 `page_mod()`
    消费后仍在 MOD；再按三连 C 才退出。
- 三连进入 MOD 时 `U.mod` 翻转，hint bar 显示 `MOD UD semi LR oct` /
  `MOD UD patch LR arp/lat`；方向消费后 MOD 保持，`held_lock` 避免同一方向
  继续触发 SEQ 的 hold pattern/BPM；长按切页/换页会清 MOD。
- C 长按 550 ms → 下一页；长按切页清 MOD，双击播放/停止不清（MOD 保持）。
- 触摸三点校准放在 **SYS → Touch Cal**：C 进入；C 在校准中取消；完成后 1.5 s
  自动退出。校准中不处理方向/按键编辑，只处理触摸和 C。
- SYN 页 C 按下/释放仍然立即 `studio_live_key()`；三连只是快速弹三个音后
  进 MOD，不会把 live note 卡住。
- `sim_input.txt` 可以按 `<ms> <mask>` 注入摇杆状态，用于回归（见 README）。

---

## 6. 硬件

### 6.1 时钟 / 音频

- HSE 12 MHz → PLL ×11 → 132 MHz；APB1 /4，APB2 /2；Flash 2 WS + prefetch。
- I2S3 主发送：`I2S_FS = 132e6 / (32*(2*37+0)) = 55 743 Hz`，相对 OPNA
  55 466.7 Hz 为 +0.5%（约 8 cent，听感可接受）。
- DMA2_Ch2 circular，ring = 1024 stereo frames（约 18 ms）；`board_audio_write()`
  在 ring 满时忙等，形成天然节流。LCD task 只在 `board_audio_ring_fill() > 512`
  （约 9 ms）时才画一帧，避免 UI 把 ring 抽干；实测 4 FM 无 UND、5 FM 开始、
  6 FM 快速上涨，所以 4 FM 是当前 UI 的干净上限。
- STW5094A 通过 I2C2 配置；数字音量在 `board_audio_write()` 里 Q15 缩放，
  耳机/扬声器都生效。

### 6.2 LCD / 输入 / MEMS / SD

- LCD：FSMC bank1 NE1，8-bit 8080 数据接 FSMC_D4..D11（每字节 `<<4`）；
  `FSMC_BTR3` 用参考的快速时序（`ADDSET=2/ADDHLD=2/DATAST=2/BUSTURN=5`）：
  面板本身没有坏线，撕裂是 GRAM 写/扫描重叠，快速写能缩短剩余窗口；
  背光 PWM 16 kHz（`TIM4_ARR=0x0FFF`）。VIS 的 128×73 频谱先画进
  `vis_cv`（SRAM），`board_lcd_canvas_end()` 用一次 window flush；这样没有
  中间空白帧，只剩 flush 本身的 tearing（TE 未接线）。
  RS=A16；`MADCTL=0xC0` 旋转 180°。
- 摇杆：PE3–PE6 + PA8，按下为高；`board_input_poll()` 返回位掩码。
- 触摸：PC0–PC3 / ADC3 注入组 ch10–13。官方 CircleOS 的 4 个按钮是
  **底部 Y>=132 的 4×32 触摸区**（`TOUCHSCR_Handler` 明确 discard buttons），
  本 UI 旋转 180°（MADCTL=0xC0），所以出现在顶部；只取顶部 **y<5**（状态栏上方 5 px），
  header 的 5..25 不参与，避免点击标题时误切页；按实测 x 顺序不再反转：
  **顶部最左→SEQ、再右→SYN、再右→MIX、最右→VIS**；SYS 走底部 tab。
  Chopstx 的 `ts_conversion` 公式
  x=128*(1000*L/(4096-U)-0x20)/0x100，y=160*(0x1e0-1000*(U-D)/(4096-U))/0x1c0；
  `board_touch_read()` 轮询，app task 40 ms 读一次，按下沿调 `ui_touch()`。
  只移动选择/页面/键位，摇杆与 C 的状态机不变。
- 三点校准：**SYS → Touch Cal** 按 C 进入；依次触摸 (115,15)/(115,115)/(15,15)
  三个十字；仿射公式参考 CircleOS `TOUCHSCR_CalculateCalibration()`（Keolabs；
  原样例由 Carlos E. Vidales 放入 public domain），算 A..F/R；
  6 个原始点存 BKP_DR2..7（DR1=0x5A5A），上电 `cal_load_raw()` 后重算矩阵。
  未校准/备份域失效时退回 Chopstx 的固定映射。
- MEMS：LIS3LV02，SPI1（PA5/6/7）+ PE2 /CS，约 1 MHz；`WHO_AM_I=0x0F`
  期望 `0x3A`；`CTRL1=0xC7`（200 Hz，三轴）；读 0x28 起 6 字节，
  `raw>>4` 得 12-bit，再 `>>1` 到约 ±1024。芯片不在时返回 -1，SYS 显示 N/A；在线时 SYS 行显示 OK x,y（原始 12-bit 值）。
- SD：参考工程的 bit-bang SD-SPI + FatFs；`FF_USE_LFN=0`（8.3 文件名），
  省掉约 100 kB 的 Unicode 表。文件固定 `/opna2.p2s`。

---

## 7. 存档格式

```c
struct st_file {                 // 1963 B, packed
  char     magic[4];             // "OPN2"
  uint8_t  ver;                  // 2（v1 旧存档自动迁移：FM4-6 置静音）
  uint8_t  bpm, swing, chain, pat;
  uint8_t  vol, bl, out, reserved;
  uint16_t song_len;             // sizeof(st_song) = 1946
  struct st_song song;
  uint16_t sum;                  // 前 1961 B 的 16-bit 和
};
```

- 保存：`app_save()` 在 app task 上先 mute，写文件，`board_audio_restart()`，
  再 unmute；写入期间音频短暂静音，避免 SD 阻塞造成 underrun。
- 读取：校验 magic/version/song_len/sum；`studio_play(0)` 清掉当前音符，
  拷贝 song、恢复速度/输出/背光，`studio_sync_song()`，再恢复播放。
- 上电：挂载 SD → 有文件则读，失败或没有则 `studio_demo_song()`。

---

## 8. 资源

### 8.1 Flash / RAM（HW）

| 项目 | 字节 |
|------|------|
| Flash load image（`size` 的 text 列） | 225 652 |
| `.text`（ELF section） | 215 908 |
| `.ramfunc`（运行时复制到 SRAM） | 9 672 |
| `.data` | 1 200 |
| `.bss` | 43 424 |
| MSP stack reserve（`stm32f103ve.ld`） | 2 048 |
| Flash 上限 | 524 288 |
| RAM 上限 | 65 536 |
| RAM 实际（data+ramfunc+bss+stack） | ≈ 55.0 kB |

`.text` 里最大的是 72 kB 字体、约 112 kB 的鼓 PCM、FatFs/FreeRTOS 与 OPNA 核心。
RAM 的大项：VIS canvas 18.7 kB、OPNA ~1.9 kB、song 1.9 kB、audio ring 4 kB、
FFT 2.5 kB、`g_file` 2 kB、UI glyph 2 kB、任务栈 4+2.5 kB、`.ramfunc` 9.7 kB。

### 8.2 QEMU

`.text` 211 460 B、`.bss` 63 872 B，其中 40 kB 是 128×160 RGB565 framebuffer。

---

## 9. QEMU 演示流水线

`studio_app_sim()` 不依赖 RTOS：

1. 画 splash → `board_lcd_present()`（PPM）。
2. `studio_init()` / `studio_demo_song()` / `board_audio_open(55466, 24*55466)`。
3. 循环渲染 256 帧 → 写 WAV；按时间轴切页/移动光标/触发 live：
   - 0–1.2 s：splash；
   - 1.2–6.5 s：SEQ，光标移动；
   - 6.5–13 s：SYN，ARP+LATCH，琴键与 tilt 扫动；
   - 13–19 s：VIS，14–16 s FFT，16–17.6 s SCOPE；
   - 19–21.5 s：MIX，静音 FM3；
   - 21.5–24 s：SYS，22.2 s 调 `app_save()` 显示 `SAVED TO SD`。
4. 每 50 ms 画一帧并 `board_lcd_present()`；24 s 共 474 帧。
5. `board_audio_close()` / `sh_exit(0)`。

视频：`ffmpeg -framerate 19.75 -i sim_shot_%03d.ppm -i sim_play_000.wav ...`；
19.75 = 474/24，保证音画同长。

---

## 10. 验证记录

- 干净目录构建：host + sim + hw 全部通过（GCC 5.4.1 / CMake 4.3.2）。
- **上板 CPU：6 FM 107%；3 FM 80–90%，暂停 57%；v1.0 默认 3 FM+SSG1
  40–50%，暂停 0%**；全部通道约 80%。
- **UND：4 FM 不涨，5 FM 开始，6 FM 快速上涨**。4 FM 是当前 UI 的干净上限；
  LCD 门限为 ring>512，`studio_render` 在 `.ramfunc`；5/6 FM 是压力模式。
- UI：输入 dirty clip；动画不再大块清屏；VIS 离屏 canvas；
  `studio_render` 进 `.ramfunc`；LCD 按 ring 余量门控。
- 默认 chain Off（SEQ 网格静态）；SYS Demo/sim 用 chain On 展示 A–D。
- 触摸/MEMS 已接入：ADC3 PC0–PC3 + LIS3LV02；触摸只移动选择，不改摇杆/C。
- 顶部 4 区按官方“底部 4×32 + 180°”映射（x/32，左→SEQ，右→VIS），页面为
  SEQ/SYN/MIX/VIS；只取顶部 5 px。上板确认 5 px 可用，header 不再误触。
  SYS → Touch Cal 进入三点校准，结果写 BKP。
- 撕裂线确认为 panel tearing；partial/dirty 渲染已去掉 25 fps 整页重绘。
- Host 24 s（3 FM + SSG1 + 鼓）：peak 18 753、0 clipped。
- QEMU 24 s（同默认 + SYN live）：474 帧、sim WAV peak 18 753、RMS 3 180、
  五页与 save 提示均出现。
- 脚本输入回归：光标/放音符/长按切页/三连 MOD/电平/静音/输出切换。
- 发现并修复：
  - `int *` 误指向 `uint8_t level[]` 导致 4 字节越界写；
  - 旧 C+方向手势在一根摇杆上难按且会卡 live voice；改为三连 C 进 MOD。
  - 切页时未清屏导致 splash 文本残留；
  - FFT 色带按柱高相对着色导致每根柱子都带红顶；
  - 示波器模式残留频谱；
  - VIS 页底部 DSP 行与 hint bar 重叠；
  - 暂停仍渲染 SSG/释放尾音（57%）；SSG 静音不旁路；
  - 25 fps 整页重绘造成整屏移动 tearing（FSMC/背光不是根因，已恢复
    参考快速时序）；改为 partial/dirty + `ui_anim()`；
  - `publish()` 的 gen 导致每次按键 `ui_draw()` 全页；改为 BSP clip rect
    的 `ui_draw_dirty()`；
  - 局部刷新时整块清屏造成闪烁；VU/键盘/倾角/网格改为差量绘制，VIS 用
    SRAM canvas；
  - `studio_render()` 在 flash 中执行；移入 `.ramfunc`，并给 LCD task 加
    `board_audio_ring_fill()` 门限。
- 校准页 `publish()` 漏拷 `cal/cal_step/cal_msg` 的 bug 已修复；QEMU 验证页面渲染。
- 未做：ADPCM-B/PPZ8、USB-MIDI、录音写 SD、电池电量。

---

## 11. 已知限制与下一步

1. **性能边界**：4 FM 是当前 UI 的稳定上限；5/6 FM 的 UND 会增长。6 FM 只作为
   压力测试模式，稳定使用请留在 4 FM 以内。
2. **6 FM 80% / 音频迟滞**：若 6 FM + UI 时 VIS 的 UND >0，优先 4/5 FM、
   关 VIS 实时 FFT、或关 SSG2/NOISE。半速率 FM 可以把 CPU 再砍一半，但
   会改变高频/音色，暂不作为默认；需要的话可做 SYS 选项。
3. **残余 tearing**：只剩切页整页和 VIS canvas flush；TE 引脚 NC，不能
   硬件同步，需要的话做 partial VIS。
4. **触摸屏**：4 线电阻屏已接 PC0–PC3 / ADC3，当前是“点选”而非拖拽；
   顶部 4 键的 x/32 顺序已上板确认。后续可做 XY pad、滑条、直接涂 grid。
5. **ADPCM-B / PPZ8**：参考核目前 stub；SD 流式窗口是可行的下一步。
6. **录音**：把 live/seq 混音写回 SD（WAV）需要更大的 RAM FIFO 或更快的
   SD 写入；18 ms ring 目前只够播放。
7. **USB-MIDI**：F103 有 USB FS；实现 device 栈后可作为桌面 MIDI 音源。
8. **电池/充电状态**：VBAT 分压到 ADC，可做电量图标。
9. **QEMU CPU%**：演示里是合成值；真机 DWT 才是真实负载。
