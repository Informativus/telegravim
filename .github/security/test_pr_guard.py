import json
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

    def test_missing_key_and_large_diff_do_not_call_provider(self):
        with patch("urllib.request.urlopen") as request:
            self.assertIn("unavailable", guard.ai_recommendation({}, "small diff", ""))
            self.assertIn(
                "unavailable", guard.ai_recommendation({}, "x" * 60_001, "key")
            )
            request.assert_not_called()

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
