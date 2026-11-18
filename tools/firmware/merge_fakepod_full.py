#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Merge the actual PlatformIO/ESP-IDF flash images into one 0x0000-based binary."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import shlex
import sys
from pathlib import Path

DEFAULT_ENV = "fakepod_s31"
OUTPUT_NAME = "FakePod_Full.bin"


def parse_int(value: str | int) -> int:
    return int(str(value).strip(), 0)


def parse_partition_offsets(partitions_csv: Path) -> dict[str, int]:
    offsets: dict[str, int] = {}
    with partitions_csv.open("r", encoding="utf-8-sig", newline="") as handle:
        for row in csv.reader(handle):
            if not row or row[0].lstrip().startswith("#") or len(row) < 4:
                continue
            name = row[0].strip()
            offset = row[3].strip()
            if name and offset:
                offsets[name] = parse_int(offset)
    return offsets


def _split_flash_args(text: str) -> list[str]:
    # posix=False preserves Windows backslashes in paths; remove only quote wrappers.
    lexer = shlex.shlex(text, posix=False)
    lexer.whitespace_split = True
    lexer.commenters = ""
    tokens = list(lexer)
    return [
        token[1:-1] if len(token) >= 2 and token[0] == token[-1] and token[0] in ("'", '"') else token
        for token in tokens
    ]


def _resolve_image_path(build_dir: Path, token: str) -> Path | None:
    raw_path = Path(token)
    candidates = [raw_path] if raw_path.is_absolute() else [build_dir / raw_path]
    candidates.append(build_dir / raw_path.name)
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


def parse_flash_args(build_dir: Path) -> list[tuple[int, Path]] | None:
    """Return the exact (offset, resolved image path) pairs, or None if no args file exists."""
    for name in ("flash_args", "flash_project_args"):
        args_path = build_dir / name
        if not args_path.is_file():
            continue

        tokens = _split_flash_args(args_path.read_text(encoding="utf-8", errors="replace"))
        entries: list[tuple[int, Path]] = []
        for index in range(len(tokens) - 1):
            try:
                offset = parse_int(tokens[index])
            except ValueError:
                continue

            image_token = tokens[index + 1]
            if not image_token.lower().endswith(".bin") or image_token.startswith("--"):
                continue

            image_path = _resolve_image_path(build_dir, image_token)
            if image_path is None:
                raise FileNotFoundError(
                    f"flash args 引用了不存在的镜像：{image_token}（文件：{args_path}）"
                )
            entries.append((offset, image_path))

        if not entries:
            raise ValueError(f"找到 {args_path}，但没有解析到任何 offset/image .bin 项")
        return entries

    return None


def _load_board_config(project_root: Path) -> tuple[int, int, int]:
    board_path = project_root / "boards" / "fakepod_s31.json"
    if not board_path.is_file():
        raise FileNotFoundError(f"找不到板卡配置：{board_path}")

    board = json.loads(board_path.read_text(encoding="utf-8"))
    upload = board.get("upload", {})
    if "bootloader_offset" not in upload or "partition_table_offset" not in upload:
        raise ValueError(
            "boards/fakepod_s31.json 必须明确配置 upload.bootloader_offset 和 "
            "upload.partition_table_offset"
        )
    if "maximum_size" not in upload:
        raise ValueError("boards/fakepod_s31.json 缺少 upload.maximum_size")

    return (
        parse_int(upload["bootloader_offset"]),
        parse_int(upload["partition_table_offset"]),
        parse_int(upload["maximum_size"]),
    )


def _find_first_image(build_dir: Path, candidates: list[str]) -> Path:
    for candidate in candidates:
        path = build_dir / candidate
        if path.is_file():
            return path.resolve()
    raise FileNotFoundError(
        "缺少编译产物，尝试过：" + ", ".join(str(build_dir / name) for name in candidates)
    )


def resolve_offsets(
    project_root: Path, build_dir: Path
) -> tuple[list[tuple[int, Path]], str, int]:
    bootloader_offset, partition_table_offset, flash_size_bytes = _load_board_config(project_root)

    partitions_csv = project_root / "partitions.csv"
    if not partitions_csv.is_file():
        raise FileNotFoundError(f"找不到分区表：{partitions_csv}")
    partition_offsets = parse_partition_offsets(partitions_csv)
    if "app0" not in partition_offsets:
        raise ValueError("partitions.csv 缺少 app0，无法安全确定应用镜像偏移")
    app_offset = partition_offsets["app0"]

    flash_images = parse_flash_args(build_dir)
    if flash_images is not None:
        bootloader_images = [
            (offset, path) for offset, path in flash_images if "bootloader" in path.name.lower()
        ]
        partition_images = [
            (offset, path) for offset, path in flash_images if "partition" in path.name.lower()
        ]
        app_images = [(offset, path) for offset, path in flash_images if offset == app_offset]

        if len(bootloader_images) != 1 or bootloader_images[0][0] != bootloader_offset:
            actual = [f"0x{offset:X}:{path}" for offset, path in bootloader_images]
            raise ValueError(
                f"Bootloader 烧录项与板卡配置不一致，期望 0x{bootloader_offset:X}，实际={actual}"
            )
        if len(partition_images) != 1 or partition_images[0][0] != partition_table_offset:
            actual = [f"0x{offset:X}:{path}" for offset, path in partition_images]
            raise ValueError(
                f"Partition Table 烧录项与板卡配置不一致，期望 0x{partition_table_offset:X}，实际={actual}"
            )
        if len(app_images) != 1:
            actual = [f"0x{offset:X}:{path}" for offset, path in flash_images]
            raise ValueError(
                f"flash args 中必须恰好有一个 app0 镜像，期望偏移 0x{app_offset:X}，实际={actual}"
            )

        if "otadata" in partition_offsets:
            ota_images = [
                (offset, path) for offset, path in flash_images
                if "ota_data" in path.name.lower() and offset == partition_offsets["otadata"]
            ]
            if len(ota_images) != 1:
                raise ValueError("分区表包含 otadata，但 flash args 中没有唯一匹配的 ota_data 镜像")

        return flash_images, "PlatformIO flash_args（实际镜像路径与偏移）", flash_size_bytes

    # 仅在没有 flash_args 文件时回退；偏移全部来自板卡配置和 partitions.csv。
    images = [
        (
            bootloader_offset,
            _find_first_image(build_dir, ["bootloader.bin", "bootloader/bootloader.bin"]),
        ),
        (
            partition_table_offset,
            _find_first_image(
                build_dir,
                ["partitions.bin", "partition_table/partition-table.bin", "partition-table.bin"],
            ),
        ),
        (
            app_offset,
            _find_first_image(build_dir, ["firmware.bin", "FakePod_S31.bin"]),
        ),
    ]
    if "otadata" in partition_offsets:
        images.append(
            (
                partition_offsets["otadata"],
                _find_first_image(build_dir, ["ota_data_initial.bin", "ota_data/ota_data_initial.bin"]),
            )
        )
    return images, "板卡配置 + partitions.csv（无 flash_args 回退）", flash_size_bytes


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def merge_images(
    images: list[tuple[int, Path]], output_path: Path, flash_size_bytes: int
) -> None:
    prepared: list[tuple[int, Path, int]] = []
    for offset, image_path in images:
        if offset < 0:
            raise ValueError(f"镜像偏移不能为负数：{image_path} offset={offset}")
        if not image_path.is_file():
            raise FileNotFoundError(f"缺少编译产物：{image_path}")
        size = image_path.stat().st_size
        if size <= 0:
            raise ValueError(f"镜像为空：{image_path}")
        prepared.append((offset, image_path, size))

    prepared.sort(key=lambda item: (item[0], str(item[1]).lower()))

    previous_end = 0
    for offset, path, size in prepared:
        if offset < previous_end:
            raise ValueError(
                f"镜像发生地址重叠：{path.name} 起始=0x{offset:X}，前一镜像结束=0x{previous_end:X}"
            )
        previous_end = offset + size
        if previous_end > flash_size_bytes:
            raise ValueError(
                f"镜像超出 Flash 容量：{path.name} 结束=0x{previous_end:X}，"
                f"Flash=0x{flash_size_bytes:X}"
            )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("wb") as output:
        current = 0
        ff_block = b"\xFF" * 65536

        for offset, image_path, _ in prepared:
            gap = offset - current
            while gap > 0:
                chunk_size = min(gap, len(ff_block))
                output.write(ff_block[:chunk_size])
                gap -= chunk_size
                current += chunk_size

            with image_path.open("rb") as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    output.write(chunk)
                    current += len(chunk)

    print("FakePod 完整固件合并完成")
    print(f"输出文件：{output_path}")
    print()
    for offset, image_path, size in prepared:
        print(
            f"  0x{offset:06X}  {image_path.name:<22} "
            f"{size:>9,} B  结束=0x{offset + size:06X}"
        )
    print()
    print(f"合并后大小：{output_path.stat().st_size:,} B (0x{output_path.stat().st_size:X})")
    print(f"SHA256：{sha256_file(output_path)}")
    print("烧录起始地址：0x0000")


def main() -> int:
    script_path = Path(__file__).resolve()
    project_root = script_path.parents[2]

    parser = argparse.ArgumentParser(description="合并 FakePod ESP32-S31 完整固件")
    parser.add_argument("--env", default=DEFAULT_ENV, help=f"PlatformIO 环境名，默认 {DEFAULT_ENV}")
    parser.add_argument(
        "--build-dir",
        type=Path,
        help="编译输出目录，默认 .pio/build/<env>",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help=f"输出文件，默认写入项目根目录/{OUTPUT_NAME}",
    )
    args = parser.parse_args()

    build_dir = (args.build_dir or (project_root / ".pio" / "build" / args.env)).resolve()
    output_path = (args.output or (project_root / OUTPUT_NAME)).resolve()

    try:
        images, source, flash_size_bytes = resolve_offsets(project_root, build_dir)
        print(f"偏移来源：{source}")
        merge_images(images, output_path, flash_size_bytes)
    except (FileNotFoundError, ValueError, OSError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
