import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "firmware" / "merge_fakepod_full.py"
SPEC = importlib.util.spec_from_file_location("merge_fakepod_full", SCRIPT)
MERGER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MERGER)


class MergeFakePodFullTests(unittest.TestCase):
    def make_project(self, root: Path) -> tuple[Path, Path]:
        (root / "boards").mkdir()
        (root / "boards" / "fakepod_s31.json").write_text(
            json.dumps({
                "upload": {
                    "bootloader_offset": "0x2000",
                    "partition_table_offset": "0x8000",
                    "maximum_size": "16777216"
                }
            }),
            encoding="utf-8",
        )
        (root / "partitions.csv").write_text(
            "# Name, Type, SubType, Offset, Size, Flags\n"
            "nvs,data,nvs,0x9000,20K,\n"
            "app0,app,ota_0,0x10000,6M,\n",
            encoding="utf-8",
        )
        build = root / ".pio" / "build" / "fakepod_s31"
        build.mkdir(parents=True)
        return root, build

    def test_flash_args_keeps_nested_source_paths_and_real_offsets(self):
        with tempfile.TemporaryDirectory() as temp:
            root, build = self.make_project(Path(temp))
            boot = build / "bootloader" / "bootloader.bin"
            partition = build / "partition_table" / "partition-table.bin"
            app = build / "FakePod_S31.bin"
            boot.parent.mkdir()
            partition.parent.mkdir()
            boot.write_bytes(b"BOOT")
            partition.write_bytes(b"PART")
            app.write_bytes(b"APP!")

            (build / "flash_args").write_text(
                "--flash_mode dio --flash_freq 80m --flash_size 16MB "
                "0x2000 bootloader/bootloader.bin "
                "0x8000 partition_table/partition-table.bin "
                "0x10000 FakePod_S31.bin",
                encoding="utf-8",
            )

            images, source, flash_size = MERGER.resolve_offsets(root, build)
            self.assertIn("flash_args", source)
            self.assertEqual(
                [(offset, path) for offset, path in images],
                [(0x2000, boot.resolve()), (0x8000, partition.resolve()), (0x10000, app.resolve())],
            )

            output = root / "FakePod_Full.bin"
            MERGER.merge_images(images, output, flash_size)
            data = output.read_bytes()
            self.assertEqual(data[0x2000:0x2004], b"BOOT")
            self.assertEqual(data[0x8000:0x8004], b"PART")
            self.assertEqual(data[0x10000:0x10004], b"APP!")
            self.assertEqual(data[:0x2000], b"\xFF" * 0x2000)

    def test_flash_args_with_wrong_bootloader_offset_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            root, build = self.make_project(Path(temp))
            (build / "bootloader.bin").write_bytes(b"BOOT")
            (build / "partitions.bin").write_bytes(b"PART")
            (build / "FakePod_S31.bin").write_bytes(b"APP!")
            (build / "flash_args").write_text(
                "0x1000 bootloader.bin 0x8000 partitions.bin 0x10000 FakePod_S31.bin",
                encoding="utf-8",
            )
            with self.assertRaises(ValueError):
                MERGER.resolve_offsets(root, build)

    def test_no_flash_args_uses_board_and_partition_configuration(self):
        with tempfile.TemporaryDirectory() as temp:
            root, build = self.make_project(Path(temp))
            (build / "bootloader.bin").write_bytes(b"BOOT")
            (build / "partitions.bin").write_bytes(b"PART")
            (build / "firmware.bin").write_bytes(b"APP!")

            images, source, flash_size = MERGER.resolve_offsets(root, build)
            self.assertIn("回退", source)
            self.assertEqual([offset for offset, _ in images], [0x2000, 0x8000, 0x10000])
            self.assertEqual(flash_size, 16 * 1024 * 1024)


if __name__ == "__main__":
    unittest.main()
