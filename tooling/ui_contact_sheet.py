# SPDX-License-Identifier: Unlicense
"""Make clearly labelled contact sheets from native renderer layout checks."""

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont  # ty: ignore[import-not-found]  # Pillow is optional contact-sheet tooling.

LABELS = dict(
    zip(
        [
            "LANGUAGE",
            "BOOT",
            "BOOT_ERROR",
            "CURRENT_TASK",
            "HOME",
            "STATUS",
            "HARDWARE",
            "SERVICES",
            "DEADLINE",
            "HOST",
            "MAINT",
            "BUILD_MENU",
            "STOCK_MENU",
            "RESTORE_MENU",
            "INPUTS",
            "TASK_DETAIL",
            "REVIEW",
            "CONFIRM",
            "RUNNING",
            "SUCCESS",
            "ERROR",
            "UNAVAILABLE",
            "STORAGE",
            "PARTITION",
            "HASH",
            "MOUNT",
            "DIAG",
            "REPORT",
            "LOGS",
            "LOG_VIEW",
            "POWER",
            "BOOT_MODE",
            "DISPLAY",
            "LAYER_EDIT",
            "EXIT",
        ],
        [
            "语言",
            "启动",
            "启动错误",
            "现有任务",
            "首页",
            "运行状态",
            "硬件库存",
            "服务状态",
            "恢复时限",
            "主机连接",
            "安装与维护",
            "构建菜单",
            "原厂资源",
            "原厂恢复",
            "已有输入",
            "任务详情",
            "核对信息",
            "确认或不可用",
            "任务执行",
            "成功状态",
            "失败状态",
            "不可用状态",
            "存储",
            "分区",
            "摘要",
            "挂载",
            "诊断",
            "报告",
            "日志分类",
            "日志视图",
            "电源",
            "启动选择",
            "显示与视差",
            "视差编辑",
            "离开恢复模式",
        ],
    )
)


def main() -> None:
    """Render labelled contact sheets from the native layout gallery."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--font", type=Path, required=True)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    font = ImageFont.truetype(str(args.font), 24)
    files = list(LABELS)
    for part in range(3):
        selected = files[part * 12 : (part + 1) * 12]
        canvas = Image.new("RGB", (1920, 1680), "#151c23")
        draw = ImageDraw.Draw(canvas)
        draw.text((24, 16), "离线布局检查 · 使用原生绘制器 · 未提交至 KMS", font=font, fill="#bdc3c7")
        for index, name in enumerate(selected):
            x, y = (index % 4) * 480, 64 + (index // 4) * 536
            with Image.open(args.input / (name + ".png")) as image:
                resized = image.resize((468, 468), Image.Resampling.LANCZOS)
                canvas.paste(resized, (x + 6, y))
            draw.text((x + 16, y + 480), LABELS[name], font=font, fill="#f1f1f2")
        canvas.save(args.out / f"离线布局概览-{part + 1}.png")


if __name__ == "__main__":
    main()
