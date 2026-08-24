import hashlib
import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

import tomllib

from tools import generate_plugin_repository as generator


class GeneratePluginRepositoryTest(unittest.TestCase):
    def test_generate_converges_repository_and_check_is_read_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            assets = root / "assets"
            libs = root / "libs"
            metadata = root / "plugin_metadata.json"
            output = root / "output"
            bundle = assets / "Example.lv2"
            bundle.mkdir(parents=True)
            libs.mkdir()
            (bundle / "manifest.ttl").write_text(
                """@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
<http://example.test/Example> lv2:binary <example.so> .
""",
                encoding="utf-8",
            )
            (bundle / "README.txt").write_bytes(b"bundle metadata")
            (libs / "libexample.so").write_bytes(b"binary payload")
            metadata.write_text(
                json.dumps(
                    {
                        "authors": {"Example": "Example Audio"},
                        "categories": {"Example": "DelayPlugin"},
                        "descriptions": {
                            "Example": "  Stereo delay for spacious guitar echoes  "
                        },
                    }
                ),
                encoding="utf-8",
            )
            (output / "README.txt").parent.mkdir(parents=True, exist_ok=True)
            (output / "README.txt").write_text("keep", encoding="utf-8")
            (output / "packages" / "stale-package").mkdir(parents=True)
            (output / "packages" / "stale-package" / "manifest.toml").write_text(
                "stale", encoding="utf-8"
            )
            (output / "payload" / "stale-package").mkdir(parents=True)
            (output / "payload" / "stale-package" / "old.zip").write_bytes(b"stale")

            with patch.object(generator, "ASSETS", assets), patch.object(
                generator, "LIBS", libs
            ), patch.object(generator, "METADATA", metadata), patch.object(
                generator, "DESCRIPTION_SOURCE", metadata
            ), patch.object(
                generator, "OUTPUT", output
            ):
                self.assertEqual(0, generator.generate())

                package = "lv2.example"
                manifest_path = output / "packages" / package / "manifest.toml"
                payload_path = output / "payload" / package / "1.0.0.zip"
                index_path = output / "index.toml"
                self.assertTrue(manifest_path.is_file())
                self.assertTrue(payload_path.is_file())
                self.assertTrue(index_path.is_file())
                self.assertEqual(
                    "keep", (output / "README.txt").read_text(encoding="utf-8")
                )
                self.assertFalse((output / "packages" / "stale-package").exists())
                self.assertFalse((output / "payload" / "stale-package").exists())

                manifest = tomllib.loads(manifest_path.read_text(encoding="utf-8"))
                index = tomllib.loads(index_path.read_text(encoding="utf-8"))
                payload = payload_path.read_bytes()
                self.assertEqual(
                    ["packages/lv2.example/manifest.toml?v=2026-08-25"],
                    index["manifests"],
                )
                self.assertEqual("lv2.example", manifest["id"])
                self.assertEqual("Example Audio", manifest["manufacturer"])
                self.assertEqual(["Delay"], manifest["tags"])
                self.assertEqual(
                    "Stereo delay for spacious guitar echoes.", manifest["description"]
                )
                self.assertNotIn("provides", manifest["description"].casefold())
                self.assertNotIn("music production", manifest["description"].casefold())
                self.assertEqual(len(payload), manifest["payload"]["size"])
                with zipfile.ZipFile(io.BytesIO(payload)) as archive:
                    self.assertEqual(
                        b"binary payload",
                        archive.read("Example.lv2/example.so"),
                    )
                    self.assertEqual(
                        b"bundle metadata",
                        archive.read("Example.lv2/README.txt"),
                    )

                before_check = self._snapshot(output)
                self.assertEqual(0, generator.generate(check=True))
                self.assertEqual(before_check, self._snapshot(output))

                stale_package_file = output / "packages" / package / "stale.txt"
                stale_package_dir = output / "packages" / package / "stale-dir"
                stale_package_file.write_text("stale", encoding="utf-8")
                stale_package_dir.mkdir()
                (stale_package_dir / "nested.txt").write_text(
                    "stale", encoding="utf-8"
                )
                stale_payload_file = output / "payload" / package / "0.9.0.zip"
                stale_payload_dir = output / "payload" / package / "stale-dir"
                stale_payload_file.write_bytes(b"stale")
                stale_payload_dir.mkdir()
                (stale_payload_dir / "nested.txt").write_text(
                    "stale", encoding="utf-8"
                )
                before_reject = self._snapshot(output)
                with self.assertRaises(SystemExit) as failure:
                    generator.generate(check=True)
                self.assertIn("packages/lv2.example", str(failure.exception))
                self.assertEqual(before_reject, self._snapshot(output))

                self.assertEqual(0, generator.generate())
                self.assertFalse(stale_package_file.exists())
                self.assertFalse(stale_package_dir.exists())
                self.assertFalse(stale_payload_file.exists())
                self.assertFalse(stale_payload_dir.exists())
                before_check = self._snapshot(output)
                self.assertEqual(0, generator.generate(check=True))
                self.assertEqual(before_check, self._snapshot(output))

    @staticmethod
    def _snapshot(root: Path):
        return {
            path.relative_to(root).as_posix(): (path.read_bytes(), path.stat().st_mtime_ns)
            for path in root.rglob("*")
            if path.is_file()
        }


if __name__ == "__main__":
    unittest.main()
