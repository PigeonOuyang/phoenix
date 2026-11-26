# 原生 Recovery 界面

使用 C++20、Cairo、FreeType、DRM/KMS 与 evdev。
普通页面使用每眼 1440 × 1280 的逻辑面板，原点为 (360, 440)；
全屏日志与报告使用默认 1970 px 宽的阅读区。双眼输出为 4320 × 2160，
当前实机显示模式为 90 Hz，窗口、列表、按钮、字体与配色使用 Breeze/Vapor 风格。

完整的 GPU 2D 渲染加速尚未接入，当前 Cairo/FreeType 主要在 CPU 上绘制。
界面基本可用，但流畅性尚未达到最优；90 Hz 扫描输出不等于稳定的 90 fps 界面绘制。
用户佩戴默认高度的标准面罩且不安装眼镜支架时，实测最大完整可见居中圆直径为
1980 px；摘下面罩后能看到更多区域。这是特定佩戴条件下的观察值，
不是显示分辨率或统一光学裁切边界，当前不按该圆软件裁切画布。

`app.cpp` 管理页面、默认取消、三键焦点、日志视图和分层视差；
`backend.cpp` 读取实际库存、状态、报告与任务回执，执行固定白名单动作；
`preferences.cpp` 管理设置事务；`main.cpp` 管理原生显示、输入与截图。
UI 由独立 inittab 服务启动，恢复时限、USB 与认证服务由既有服务继续管理。

## 交互

音量键移动选中项，默认音量＋向上，可在“设置 → 按键操作”反转；视差编辑时仍为音量＋增加、音量－减少。
电源短按并松开选择；如果按住期间焦点、页面或任务状态已变，松开不执行另一个动作。选中当前任务概览后，电源进入日志视图，
默认焦点为“返回”。日志正文中电源先选中“返回”，再次电源返回原页面及原焦点。
不提供长按两秒退出手势，内核原有电源长按行为继续生效。

状态栏在每个页面显示。时间和日期位于固定中间区域；电量与功率在右侧。
左侧蓝牙、Wi-Fi 和有线网无可信观测时显示灰色状态符号，不推断或启动无线功能。
少按钮任务页的底部操作组水平居中，保留按钮尺寸与间距。
按钮及返回文字按实际字体墨迹边界在容器内居中，列表行仍使用原有排版。
当前任务概览保持原有显示范围。
7 段主机安装、9 段设备端构建安装及 7 段恢复流程来自 `task-state` 和 `devloop`；
全屏安装日志在正文下显示进度条，并在当前阶段展开子步骤。主条按子步骤的预估耗时分配长度；
设备计数和完成回执推动实色填充，无可信计数时使用活动条纹。展开与收起不移动日志正文。
只读身份查询不创建安装任务。

视差为 `xL - xR = d`，每层分别偏移 `+d/2` 和 `-d/2`。
无视差时直接复制左眼像素，保证双眼逐字节相同。视差参数仅在本次会话生效。

## 设置、图标与持久存储

支持 `zh-CN` 与 `en`，分区名、数据和原始诊断保留原值。
首次确认通过异步事务保存，成功读取持久回执后进入日志视图。失败可重试。
后续从首页“设置”更改状态项、时间与日期、超时、流量单位、语言和按键方向。视图偏好即时预览；
超时只接收已保存且回读通过的值。连接、测试计时继续沿用原规则，数值只允许大于上游默认值或选择“不限制”。
更改已到期的时限时要求再次确认。当前页下拉和确认分别使用 220 / 250 ms 淡入淡出，
浮层相对同层控件增加 12 px 水平视差。`P4_REDUCED_MOTION=1` 可关闭过渡。

图标从固定提交的官方 Breeze SVG 生成掩码图集，运行时只依赖原有 Cairo 和 libpng。
原图、清单、署名和 LGPL 许可证见 `breeze/`。重建时运行
`python3 tooling/ui_breeze.py src/recovery-ui/breeze src/recovery-ui/breeze`（构建主机需要 librsvg 和 libcairo）。

兼容旧版 `{"version":1,"locale":"zh-CN","language_selected":true}`，新增 `settings` 固定字段。
读取严格验证版本和类型，未知字段保留。这里保存的是 Recovery 界面偏好；
当前改动没有为 SteamOS 新增读取此偏好的开机语言同步服务。

已有 `pico4-data` 的位置为 `var/lib/pico4/recovery/preferences.json`。
安装前使用缓存分区的 `pico4-recovery/preferences.json`。
数据分区配置优先；数据分区重建后，已有缓存选择继续有效，
下次明确保存再迁移，保留缓存副本。加载过程只读。

事务与安装流程共享同一个 `install.lock`。临时文件同步、原子重命名、
父目录同步、卸载与分区/父盘恢复只读后，再进行新的只读挂载回读。
不覆盖其他文件，不写工厂 persist，不自动修复文件系统。
缓存分区根目录可使用已核验的 Android `system:cache 0770` 模式，不改变其权限。
新建配置目录与文件在上游 `022` 掩码下分别为 root 所有的 `0755` 和 `0644`；
已有目录不会自动改权限。任务锁、令牌、日志及临时对象按对象单独保护，
不把私有掩码传递给安装器或系统构建。

## 状态接口

电量、充电状态读取 `power_supply/battery` 的实际节点。功率只在存在
`current_now`、`voltage_now` 及明确电流极性标记时显示，单位转换为微安 × 微伏 / 10¹²。
`/run/overpressure/battery-current-direction` 接受
`positive-is-charging` 或 `positive-is-discharging`。正向净功率为绿色 `+xxW`，
负向为红色 `-xxW`。当前内核缺少电流和电压节点，显示“未知”。

日期和时间直接读取系统时钟，状态栏不显示星期；1970 年等未初始化时钟仍显示“时间未校准”。
显示支持 2020–2099 年，不再要求一个没有生成程序的联网校时标记。
时区优先使用进程已有 `TZ`、`/etc/TZ`、可解析的 `/etc/timezone` 和 `/etc/localtime`。

开发工具认证连接、核对镜像后，使用 `pico4-time` 发送电脑当前 UTC 时间及本地时区偏移，
并回读系统时钟和启动实例。UI 每秒重新读取 `/run/overpressure/host-timezone`，无需重启。
`/run/overpressure/time-source.json` 记录实际来源为 `host`，不冒充 NTP 校时。
电脑偏移仅用于缺少系统时区设置时的回退，代表电脑当前偏移，不包含未来夏令时规则；
再次连接会更新。NCM 默认只有电脑与头显间的局域网，没有默认网关或 DNS。
校时仅修改本次运行的系统时钟和 `/run`，不写 RTC、工厂分区或语言偏好。
无电脑连接时，已有初始化时钟仍可显示；原始 RTC 为 1970 年时需要重新校时。

## 截图与诊断

认证终端执行 `pico4-ui-shot`，返回已完成 KMS 提交的帧和 `/run/overpressure/screenshots/` 路径。
目录包括双眼、左眼、右眼 PNG 与截图信息 JSON。PNG 编码异步进行，
截图来源是刚完成提交的缓冲，不改变当前页面。截图 socket 仅接受 `capture`，
权限为 `0600`，不提供远程导航或任意命令。
服务端等待已连接客户端的请求就绪，非阻塞轮询最长一秒，避免读取先于发送造成断管。
`--screenshot-delay-test` 在连接后延迟 250 毫秒再发送同一截图请求，用于验证此时序。

`--input-inventory` 读取真实 evdev 能力；`--input-selftest` 为合成输入过滤检查。
`--render-gallery DIRECTORY` 生成离线布局图，不提交 KMS，不执行按钮动作。
离线图采用无视差模式，附带字体边界核对信息和明确标注的额外任务状态样例；
`tooling/ui_alignment.py` 测量实际 PNG 中的文字对齐误差，不将布局样例作为硬件任务结果。
实机验证工具的按键通过 evdev 合成注入，不能证明实体按键触感或面板光学效果。

## 构建与验证

`tooling/ui_sysroot.py` 从签名校验的 Alpine ARM64 包准备运行依赖与 musl sysroot；
`tooling/ui_fonts.py` 获取完整的 Noto CJK SC 字体。不能用设计稿 WOFF 子集替代完整字体。

```sh
bash scripts/build-recovery-ui.sh ALPINE_SYSROOT OUTPUT
bash scripts/test-recovery-ui.sh TEST_OUTPUT
bash scripts/test-recovery-clock.sh TEST_OUTPUT
bash scripts/test-recovery-preferences.sh TEST_OUTPUT JSONC_INCLUDE
sh tests/test_recovery_status.sh
python tooling/ui_regression.py
```

`tooling/ui_image.py --base BASE --binary BINARY --runtime RUNTIME_CPIO --fonts FONTS --out IMAGE`
追加运行库、UI、字体与源码覆盖层，核对内核、DTB、DTBO、命令行保持一致并保留原 AVB。
基础镜像的认证与开发时限也保留。以后替换内核时，可把包含 UI 的开发镜像继续作为
`kernel/recovery_image.py` 的基底；仅添加 stage1 脚本不等于已安装 UI 二进制和运行库。
