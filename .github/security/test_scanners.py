import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import pr_guard as guard


@unittest.skipUnless(
    os.environ.get("SCANNER_BIN"), "verified scanner binaries are not installed"
)
class ScannerBoundaryTests(unittest.TestCase):
    def test_inline_secret_allow_directive_does_not_hide_a_leak(self):
        sample = (
            'github_token = "'
            + "ghp_"
            + "A3b4C5d6E7f8G9h0I1j2K3l4M5n6O7p8Q9r0"
            + '" # gitleaks:allow\n'
        )
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(guard.GuardError):
                guard.scanner(
                    Path(directory),
                    "gitleaks",
                    [
                        "stdin",
                        "--config",
                        str(guard.HERE / "gitleaks.toml"),
                        "--gitleaks-ignore-path",
                        "/dev/null",
                        "--ignore-gitleaks-allow",
                        "--redact=100",
                        "--no-banner",
                    ],
                    sample.encode(),
                )

    def test_another_api_hash_is_not_covered_by_public_upstream_exception(self):
        sample = b"TDESKTOP_API_HASH=" + b"97bd52a0e38fc614b8ca29065d47ef13"
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(guard.GuardError):
                guard.scanner(
                    Path(directory),
                    "gitleaks",
                    [
                        "stdin",
                        "--config",
                        str(guard.HERE / "gitleaks.toml"),
                        "--gitleaks-ignore-path",
                        "/dev/null",
                        "--ignore-gitleaks-allow",
                        "--redact=100",
                        "--no-banner",
                    ],
                    sample,
                )

    def test_zizmor_inline_suppression_and_pr_config_are_ignored(self):
        content = """name: Test
on: pull_request_target # zizmor: ignore[dangerous-triggers]
permissions: {}
jobs:
  test:
    runs-on: ubuntu-24.04
    steps:
      - run: echo hello
"""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / ".github/workflows/untrusted.yml"
            path.parent.mkdir(parents=True)
            path.write_text(content)
            (root / "zizmor.yml").write_text(
                "rules:\n  dangerous-triggers:\n    disable: true\n"
            )
            with self.assertRaises(guard.GuardError):
                guard.check_workflows(root, [str(path)])

    def test_history_scan_catches_secret_added_then_removed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo, inspection = root / "repo", root / "inspection"
            repo.mkdir()
            inspection.mkdir()

            def git(*args):
                return (
                    subprocess.check_output(
                        ["git", "-C", str(repo), *args], stderr=subprocess.DEVNULL
                    )
                    .decode()
                    .strip()
                )

            git("init")
            git("config", "user.email", "security-test@example.invalid")
            git("config", "user.name", "Security Test")
            path = repo / "code.cpp"
            path.write_text("int value = 1;\n")
            git("add", ".")
            git("commit", "-m", "baseline")
            base = git("rev-parse", "HEAD")
            path.write_text(
                'const char *token = "'
                + "ghp_"
                + "A3b4C5d6E7f8G9h0I1j2K3l4M5n6O7p8Q9r0"
                + '";\n'
            )
            git("add", ".")
            git("commit", "-m", "add secret")
            path.write_text("int value = 2;\n")
            git("add", ".")
            git("commit", "-m", "remove secret")
            head = git("rev-parse", "HEAD")
            result = guard.inspect(repo, inspection, head, base, base)
            self.assertFalse(result["scan_ok"])
            self.assertTrue(result["reasons"])


if __name__ == "__main__":
    unittest.main()
