# KDE Breeze 图标资源

原始图标来自 [KDE/breeze-icons](https://github.com/KDE/breeze-icons/tree/744be60c294e79719c7506ead95f1f103e539993)，固定提交 `744be60c294e79719c7506ead95f1f103e539993`。Copyright © 2014 Uri Herrera and others。图标库按 **LGPL-3.0-or-later** 提供；[COPYING-ICONS](COPYING-ICONS) 包含完整 LGPL v3 文本、署名及在 GUI 中使用图标的澄清，[GPL-3.0.txt](GPL-3.0.txt) 是被引用的 GPL v3 全文。

[manifest.json](manifest.json) 逐项列出上游名称、别名解析、固定提交、实际源文件和 SHA-256。`original/` 中保存原始 SVG。`tooling/ui_breeze.py` 从这些 SVG 生成 `atlas.png` 和 `breeze_index.hpp`。PNG 四通道存储独立的颜色角色覆盖率，Cairo 按当前 Vapor 色角色显示；没有修改路径、图形坐标、线宽或内部变换。

Recovery 使用官方网络状态符号表示 USB NCM 状态。电量未知用 `question` 配合未知文本，和实测 `0%` 或已确认不存在区分。没有拼贴徽标或引入其他图标库。
