import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import pr_guard as guard


class ClassificationTests(unittest.TestCase):
    def classify(
        self,
        path="Telegram/SourceFiles/new.cpp",
        before=b"",
        after=b"int value;",
        old_mode="100644",
        new_mode="100644",
        upstream=None,
    ):
        return guard.classify(
            path, old_mode, new_mode, "1" * 40, "2" * 40, upstream, before, after
        )

    def test_official_content_at_same_path_and_mode_is_exempt(self):
        self.assertFalse(
            self.classify(
                after=b"QNetworkRequest request;", upstream=("100644", "2" * 40)
            )
        )

    def test_official_content_at_another_path_is_not_exempt(self):
        self.assertTrue(self.classify(after=b"QNetworkRequest request;"))

    def test_same_blob_with_different_mode_is_not_exempt(self):
        self.assertTrue(self.classify(new_mode="100755", upstream=("100644", "2" * 40)))

    def test_official_control_files_still_require_owner(self):
        self.assertTrue(
            self.classify(
                path=".github/workflows/test.yml", upstream=("100644", "2" * 40)
            )
        )

    def test_existing_network_context_is_reviewed_even_if_added_line_is_harmless(self):
        self.assertIn(
            "Network-related", self.classify(before=b"QNetworkAccessManager manager;")
        )

    def test_unchanged_copyright_url_does_not_claim_network_code(self):
        header = b"// https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL\n"
        self.assertIn(
            "cannot be excluded",
            self.classify(
                before=header + b"int value = 1;", after=header + b"int value = 2;"
            ),
        )

    def test_changed_endpoint_is_explicitly_network_related(self):
        self.assertIn(
            "Network-related",
            self.classify(
                before=b'auto url = "https://old.example";',
                after=b'auto url = "https://new.example";',
            ),
        )

    def test_custom_code_without_detectable_network_fails_closed(self):
        self.assertIn("cannot be excluded", self.classify(after=b"opaqueHelper();"))

    def test_removing_custom_network_code_is_reviewed(self):
        self.assertTrue(
            self.classify(new_mode="000000", before=b"fetch(endpoint);", after=b"")
        )

    def test_markdown_in_code_directory_is_not_automatically_exempt(self):
        self.assertTrue(self.classify(path="Telegram/generated.md"))

    def test_plain_documentation_is_exempt(self):
        self.assertFalse(
            self.classify(path="docs/usage.md", after=b"Use https://example.com")
        )

    def test_executable_docs_symlinks_and_submodules_are_not_exempt(self):
        for mode in ["100755", "120000", "160000"]:
            with self.subTest(mode=mode):
                self.assertTrue(self.classify(path="docs/usage.md", new_mode=mode))

    def test_control_file_deletion_is_not_exempt(self):
        self.assertTrue(
            self.classify(path=".github/security/policy.json", new_mode="000000")
        )


class CodeQLSelectionTests(unittest.TestCase):
    def test_documentation_and_ci_without_cpp_do_not_scan_unchanged_cpp(self):
        self.assertFalse(
            guard.codeql_required(
                ["docs/pr-security.md", ".github/security/pr_guard.py"]
            )
        )

    def test_sources_build_files_and_dependency_changes_require_codeql(self):
        for path in [
            "Telegram/main.cpp",
            ".github/example.cpp",
            "CMakeLists.txt",
            ".gitmodules",
            "cmake/deps.cmake",
        ]:
            with self.subTest(path=path):
                self.assertTrue(guard.codeql_required([path]))


class SecurityResultTests(unittest.TestCase):
    def test_codeql_skip_only_passes_when_explicitly_not_required(self):
        class API:
            def repo(self, suffix):
                if suffix.startswith("pulls/"):
                    return {
                        "state": "open",
                        "head": {"sha": "a" * 40},
                        "base": {
                            "ref": "develop",
                            "sha": "b" * 40,
                            "repo": {"full_name": guard.POLICY["repository"]},
                        },
                    }
                if suffix == "branches/develop":
                    return {"commit": {"sha": "b" * 40}}
                if suffix == "commits/main":
                    return {"sha": "c" * 40}
                raise AssertionError(suffix)

            def status(self, head, context, state, detail):
                self.state = state

        for required, result, expected in [
            ("false", "skipped", True),
            ("true", "success", True),
            ("true", "skipped", False),
            ("", "skipped", False),
            ("false", "failure", False),
            ("true", "cancelled", False),
        ]:
            with (
                self.subTest(required=required, result=result),
                patch.dict(
                    os.environ,
                    {
                        "PR_HEAD": "a" * 40,
                        "PR_BASE": "b" * 40,
                        "PR_NUMBER": "17",
                        "CONTROL_SHA": "c" * 40,
                        "AUDIT_RESULT": "success",
                        "SCAN_OK": "true",
                        "CODEQL_REQUIRED": required,
                        "CODEQL_RESULT": result,
                    },
                ),
            ):
                api = API()
                if expected:
                    guard.finish(api, "security")
                    self.assertEqual(api.state, "success")
                else:
                    with self.assertRaises(guard.GuardError):
                        guard.finish(api, "security")
                    self.assertEqual(api.state, "failure")


class BoundaryTests(unittest.TestCase):
    def test_paths_cannot_escape_export_root(self):
        for path in [
            "../key",
            "/etc/passwd",
            ".git/config",
            "a/.git/config",
            "a\\b",
            "a\nfile",
        ]:
            with self.subTest(path=path), self.assertRaises(guard.GuardError):
                guard.safe_path(path)

    def test_sha_is_not_a_git_option_or_revision_expression(self):
        for value in ["--upload-pack=bad", "main", "a" * 39, "a" * 40 + ":file"]:
            with self.subTest(value=value), self.assertRaises(guard.GuardError):
                guard.valid_sha(value)

    def test_new_head_or_base_invalidates_approval(self):
        state = {"head": "a" * 40, "base": "b" * 40}
        self.assertTrue(
            guard.same_snapshot(
                {"head": {"sha": state["head"]}, "base": {"sha": state["base"]}}, state
            )
        )
        for field in ["head", "base"]:
            pr = {"head": {"sha": state["head"]}, "base": {"sha": state["base"]}}
            pr[field]["sha"] = "c" * 40
            self.assertFalse(guard.same_snapshot(pr, state))

    def test_current_pr_uses_live_branch_tip_instead_of_stale_pr_base(self):
        class API:
            def repo(self, suffix):
                if suffix == "pulls/11":
                    return {
                        "state": "open",
                        "head": {"sha": "a" * 40},
                        "base": {
                            "ref": "develop",
                            "sha": "b" * 40,
                            "repo": {"full_name": guard.POLICY["repository"]},
                        },
                    }
                if suffix == "branches/develop":
                    return {"commit": {"sha": "c" * 40}}
                raise AssertionError(suffix)

        self.assertEqual(guard.current_pr(API(), 11)["base"]["sha"], "c" * 40)

    def test_comment_output_cannot_inject_marker_or_mention(self):
        text = guard.safe_text("@owner\n<!-- forged -->`x`")
        self.assertNotIn("@", text)
        self.assertNotIn("<", text)
        self.assertNotIn("`", text)
        self.assertNotIn("\n", text)

    def test_sarif_missing_results_does_not_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(guard.GuardError):
                guard.check_sarif(Path(directory))

    def test_sarif_findings_and_failed_invocations_block(self):
        for run in [
            {"results": [{"level": "error"}]},
            {"invocations": [{"executionSuccessful": False}]},
        ]:
            with self.subTest(run=run), tempfile.TemporaryDirectory() as directory:
                Path(directory, "result.sarif").write_text(json.dumps({"runs": [run]}))
                with self.assertRaises(guard.GuardError):
                    guard.check_sarif(Path(directory))


if __name__ == "__main__":
    unittest.main()


class EnvironmentTests(unittest.TestCase):
    def environment(self):
        return {
            "can_admins_bypass": False,
            "protection_rules": [
                {
                    "type": "required_reviewers",
                    "prevent_self_review": False,
                    "reviewers": [
                        {"type": "User", "reviewer": {"login": "Informativus"}}
                    ],
                }
            ],
        }

    def verify(self, environment):
        class API:
            def repo(self, suffix):
                return environment

        guard.verify_environment(API())

    def test_only_owner_environment_with_self_review_is_accepted(self):
        self.verify(self.environment())

    def test_empty_or_wrong_reviewer_does_not_pass(self):
        for reviewers in [
            [],
            [{"type": "User", "reviewer": {"login": "someone-else"}}],
        ]:
            environment = self.environment()
            environment["protection_rules"][0]["reviewers"] = reviewers
            with self.subTest(reviewers=reviewers), self.assertRaises(guard.GuardError):
                self.verify(environment)

    def test_additional_reviewer_cannot_approve_instead_of_owner(self):
        environment = self.environment()
        environment["protection_rules"][0]["reviewers"].append(
            {"type": "User", "reviewer": {"login": "someone-else"}}
        )
        with self.assertRaises(guard.GuardError):
            self.verify(environment)

    def test_admin_bypass_is_rejected(self):
        environment = self.environment()
        environment["can_admins_bypass"] = True
        with self.assertRaises(guard.GuardError):
            self.verify(environment)

    def test_self_review_prohibition_is_rejected(self):
        environment = self.environment()
        environment["protection_rules"][0]["prevent_self_review"] = True
        with self.assertRaises(guard.GuardError):
            self.verify(environment)


class ExportScopeTests(unittest.TestCase):
    def setUp(self):
        self.entries = {
            "Telegram/SourceFiles/a.cpp": ("100644", "a" * 40),
            "Telegram/SourceFiles/b.cpp": ("100644", "b" * 40),
            "Telegram/SourceFiles/common.h": ("100644", "c" * 40),
            "Telegram/SourceFiles/nested.h": ("100644", "d" * 40),
        }
        self.blobs = {
            "a" * 40: b'#include "common.h"\n',
            "b" * 40: b"int b;",
            "c" * 40: b'#include "nested.h"\n',
            "d" * 40: b"int helper();",
        }

    def paths(self, changed):
        with (
            patch.object(guard, "tree", return_value=self.entries),
            patch.object(
                guard,
                "read_blob",
                side_effect=lambda repo, blob, limit: self.blobs[blob],
            ),
        ):
            return guard.export_paths(Path("/unused"), "a" * 40, changed)[0]

    def test_changed_translation_unit_keeps_recursive_headers(self):
        result = self.paths(["Telegram/SourceFiles/a.cpp"])
        self.assertEqual(
            set(result), set(self.entries) - {"Telegram/SourceFiles/b.cpp"}
        )

    def test_header_and_build_changes_retain_complete_tree(self):
        for path in ["Telegram/SourceFiles/common.h", "CMakeLists.txt", ".gitmodules"]:
            with self.subTest(path=path):
                self.assertEqual(self.paths([path]), self.entries)

    def test_deleted_translation_unit_falls_back_to_full_tree(self):
        self.assertEqual(self.paths(["Telegram/SourceFiles/deleted.cpp"]), self.entries)
