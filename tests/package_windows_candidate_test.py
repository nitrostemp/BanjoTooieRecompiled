"""Focused packager checks: documentation refresh and release-mode guards."""
import importlib.util
from pathlib import Path
import json
import sys
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "packager", Path(__file__).resolve().parents[1] / "tools/package_windows_candidate.py")
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)

BASE = "a" * 40
HEAD = "b" * 40
RECEIPT = {"sourceCommit": BASE, "sourceDirty": False,
           "revisionAtConfigure": BASE[:12], "gitStateAtConfigure": "clean"}


class DocumentationRefresh(unittest.TestCase):
    def revision(self, changed, state="clean", receipt=RECEIPT):
        with patch.object(packager, "run", side_effect=[BASE, "", changed]):
            return packager.documentation_source_revision(receipt, HEAD, state)

    def test_documentation_preserves_binary_revision(self):
        self.assertEqual(self.revision(
            "docs/WINDOWS.md\0.github/ISSUE_TEMPLATE/bug_report.md\0"
            "third_party/notices/RT64-imgui.txt\0"
            "tools/package_windows_candidate.py\0tests/package_windows_candidate_test.py\0"), BASE)

    def test_runtime_source_dependency_asset_and_allowlist_changes_rejected(self):
        for path in ("src/main.cpp", "dependencies.lock.json", "assets/recomp.rcss",
                     "release/windows-files.json", "CMakeLists.txt"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                self.revision("docs/WINDOWS.md\0" + path + "\0")

    def test_dirty_or_misidentified_baseline_rejected(self):
        with self.assertRaises(ValueError):
            self.revision("", state="dirty")
        with self.assertRaises(ValueError):
            self.revision("", receipt={**RECEIPT, "revisionAtConfigure": HEAD[:12]})

    def test_payload_only_documentation_may_change(self):
        before = {"TooieRecompiled.exe": "exe", "runtime-data/id/manifest.json": "meta",
                  "docs/RELEASE_NOTES.md": "old"}
        after = {**before, "docs/RELEASE_NOTES.md": "new"}
        packager.verify_documentation_payload(before, after)
        for name in ("TooieRecompiled.exe", "runtime-data/id/manifest.json"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                packager.verify_documentation_payload(before, {**after, name: "changed"})
        with self.assertRaises(ValueError):
            packager.verify_documentation_payload(before, {**after, "extra.dll": "new"})


class PackageContents(unittest.TestCase):
    def test_allowlist_is_player_facing(self):
        allowlist = json.loads(packager.ALLOWLIST.read_text(encoding="utf-8"))
        paths = [str(path) for path in packager.expected_paths(allowlist, "0" * 64)]
        self.assertIn("README.md", paths)
        self.assertIn("docs/PLAYER_GUIDE.md", paths)
        self.assertIn("docs/RELEASE_NOTES.md", paths)
        self.assertNotIn("release/windows-files.json", paths)
        docs = [path for path in paths if path.startswith("docs/")]
        self.assertEqual(sorted(docs), ["docs/PLAYER_GUIDE.md", "docs/RELEASE_NOTES.md"])
        self.assertIn("third_party/notices/DXC-v1.8.2502-LICENSE-LLVM.txt", paths)
        self.assertIn("third_party/notices/FONT_COPYRIGHTS.txt", paths)
        self.assertIn("third_party/notices/X-Scale-CIC-BSD.txt", paths)
        self.assertNotIn("third_party/notices/RmlUi-MIT.txt", paths)
        self.assertFalse(any("SOURCES.md" in path or "DXC-v1.7" in path for path in paths))
        self.assertFalse(any(path.startswith("docs/licenses/") for path in paths))
        for path in paths:
            if path.startswith(("docs/", "assets/", "third_party/notices/")) or path in {"README.md", "LICENSE", "THIRD_PARTY_NOTICES.md"}:
                self.assertTrue(packager.source_for(packager.safe_relative(path), packager.ROOT / "build").is_file(), path)

    def test_package_readme_comes_from_player_template(self):
        source = packager.source_for(packager.safe_relative("README.md"), packager.ROOT / "build")
        self.assertEqual(source, packager.ROOT / "release" / "README.player.md")

    def test_unknown_allowlist_schema_rejected(self):
        with self.assertRaises(ValueError):
            packager.expected_paths({"schema": 1, "files": []}, "0" * 64)


class ReleaseGuards(unittest.TestCase):
    def main(self, *arguments):
        with patch.object(sys, "argv", ["package_windows_candidate.py", *arguments]):
            packager.main()

    def test_release_refuses_candidate_relabel_and_unsafe_modes(self):
        for arguments in (["--release", "--refresh"], ["--release", "--docs-only", "--verify-only"],
                          ["--release", "--allow-dirty"], ["--release", "--sdl-observer-sha256", "a" * 64],
                          ["--release", "--output-dir", str(packager.DEFAULT_CANDIDATE)]):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                self.main(*arguments)

    def test_release_requires_explicit_version_approval(self):
        allowlist = {"schema": 2, "candidateVersion": "9.9.9", "releaseApproval": None, "files": []}
        with patch.object(packager, "read_json", return_value=allowlist),                 patch.object(packager, "run", side_effect=AssertionError("git must not run")):
            with self.assertRaisesRegex(ValueError, "releaseApproval"):
                self.main("--release", "--output-dir", str(packager.ROOT / "dist" / "release-test"))


if __name__ == "__main__":
    unittest.main()
