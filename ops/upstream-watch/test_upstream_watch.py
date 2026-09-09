import contextlib
import io
import json
import tempfile
import unittest
import urllib.error
from pathlib import Path
from unittest.mock import Mock, patch

import upstream_watch as watch


class UpstreamWatchTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.config = {
            "baseline_version": "7.1.3",
            "ntfy_url": "https://ntfy.example.com",
            "ntfy_topic": "telegravim-updates",
            "ntfy_token_file": self.root / "ntfy.token",
            "state_file": self.root / "state" / "state.json",
        }
        self.config_path = self.root / "config.json"
        self.config_path.write_text(
            json.dumps({key: str(value) for key, value in self.config.items()})
        )

    def write_token(self):
        path = self.config["ntfy_token_file"]
        path.write_text("test-secret\n")
        path.chmod(0o600)

    def test_numeric_versions_and_invalid_tags(self):
        self.assertGreater(watch.version("v7.10.0"), watch.version("7.9.9"))
        for invalid in (
            None,
            123,
            "",
            "v7.2.5-beta",
            "7.2",
            "7.2.5\n",
            "../../test",
            "7.2.5;exit",
            "7.2.9999999",
        ):
            with self.subTest(value=invalid), self.assertRaises(watch.WatchError):
                watch.version(invalid)

    def test_only_stable_release_metadata_is_accepted(self):
        for release in (
            [],
            {},
            {"tag_name": "v7.2.5"},
            {"tag_name": "v7.2.5", "draft": True, "prerelease": False},
            {"tag_name": "v7.2.5", "draft": False, "prerelease": True},
        ):
            with (
                self.subTest(release=release),
                patch.object(watch, "request_json", return_value=release),
                self.assertRaises(watch.WatchError),
            ):
                watch.fetch_release()
        release = {
            "tag_name": "v7.2.5",
            "draft": False,
            "prerelease": False,
            "html_url": "https://invalid.example",
        }
        with patch.object(watch, "request_json", return_value=release):
            self.assertEqual(watch.fetch_release(), "v7.2.5")

    def test_dry_run_does_not_read_token_or_create_state_directory(self):
        with (
            patch.object(watch, "fetch_release", return_value="v7.2.5"),
            patch.object(watch, "send_notification") as send,
        ):
            result = watch.check(self.config, dry_run=True)
        self.assertEqual(result["status"], "update_available")
        self.assertEqual(result["url"], watch.RELEASE_PAGE + "v7.2.5")
        self.assertFalse(self.config["state_file"].parent.exists())
        send.assert_not_called()

    def test_successful_delivery_is_remembered_across_runs(self):
        with (
            patch.object(watch, "fetch_release", return_value="v7.2.5"),
            patch.object(watch, "send_notification") as send,
        ):
            with watch.locked_state(self.config["state_file"]):
                self.assertEqual(watch.check(self.config)["status"], "notified")
            with watch.locked_state(self.config["state_file"]):
                self.assertEqual(watch.check(self.config)["status"], "no_update")
            send.assert_called_once_with(self.config, "v7.2.5")
        state = watch.read_json(self.config["state_file"])
        self.assertEqual(state["notified_tag"], "v7.2.5")
        self.assertEqual(self.config["state_file"].stat().st_mode & 0o777, 0o600)

    def test_same_or_older_versions_do_not_notify(self):
        for tag in ("v7.1.3", "v7.1.2", "v6.9.9"):
            with (
                self.subTest(tag=tag),
                patch.object(watch, "fetch_release", return_value=tag),
                patch.object(watch, "send_notification") as send,
            ):
                self.assertEqual(watch.check(self.config)["status"], "no_update")
                send.assert_not_called()

    def test_raising_baseline_does_not_trigger_old_release(self):
        self.config["state_file"].parent.mkdir()
        watch.save_state(self.config["state_file"], "v7.2.5")
        self.config["baseline_version"] = "7.4.0"
        self.assertEqual(watch.threshold(self.config), (7, 4, 0))

    def test_network_failure_does_not_advance_state(self):
        with (
            patch.object(
                watch, "fetch_release", side_effect=watch.WatchError("Unavailable")
            ),
            patch.object(watch, "send_notification") as send,
        ):
            with self.assertRaises(watch.WatchError):
                watch.check(self.config)
            send.assert_not_called()
        self.assertFalse(self.config["state_file"].exists())

    def test_failed_delivery_is_retried(self):
        self.config["state_file"].parent.mkdir()
        watch.save_state(self.config["state_file"], "v7.1.4")
        original = self.config["state_file"].read_bytes()
        with (
            patch.object(watch, "fetch_release", return_value="v7.2.5"),
            patch.object(
                watch,
                "send_notification",
                side_effect=[watch.WatchError("Unavailable"), None],
            ) as send,
        ):
            with self.assertRaises(watch.WatchError):
                watch.check(self.config)
            self.assertEqual(self.config["state_file"].read_bytes(), original)
            self.assertEqual(watch.check(self.config)["status"], "notified")
            self.assertEqual(send.call_count, 2)

    def test_corrupt_state_is_not_overwritten(self):
        self.config["state_file"].parent.mkdir()
        for raw in (
            "invalid",
            "[]",
            '{"schema_version":2}',
            '{"schema_version":1,"notified_tag":"beta"}',
        ):
            self.config["state_file"].write_text(raw)
            with self.subTest(raw=raw), patch.object(watch, "fetch_release") as fetch:
                with self.assertRaises(watch.WatchError):
                    watch.check(self.config)
                fetch.assert_not_called()
                self.assertEqual(self.config["state_file"].read_text(), raw)

    def test_atomic_save_failure_preserves_previous_state(self):
        self.config["state_file"].parent.mkdir()
        watch.save_state(self.config["state_file"], "v7.1.4")
        original = self.config["state_file"].read_bytes()
        with (
            patch.object(Path, "replace", side_effect=OSError("Disk error")),
            self.assertRaises(OSError),
        ):
            watch.save_state(self.config["state_file"], "v7.2.5")
        self.assertEqual(self.config["state_file"].read_bytes(), original)
        self.assertEqual(
            list(self.config["state_file"].parent.iterdir()),
            [self.config["state_file"]],
        )

    def test_overlapping_checks_are_skipped(self):
        with (
            watch.locked_state(self.config["state_file"]) as first,
            watch.locked_state(self.config["state_file"]) as second,
        ):
            self.assertTrue(first)
            self.assertFalse(second)
        with watch.locked_state(self.config["state_file"]) as third:
            self.assertTrue(third)

    def test_config_rejects_typos_relative_paths_and_shared_files(self):
        cases = (
            {"unknown_key": "value"},
            {"state_file": "relative.json"},
            {"ntfy_token_file": str(self.config["state_file"])},
            {"ntfy_topic": "../private"},
            {"ntfy_topic": "a,b"},
            {"ntfy_topic": ""},
            {"state_file": str(self.config_path)},
            {"baseline_version": "beta"},
        )
        original = watch.read_json(self.config_path)
        for changes in cases:
            self.config_path.write_text(json.dumps(original | changes))
            with self.subTest(changes=changes), self.assertRaises(watch.WatchError):
                watch.read_config(self.config_path)

    def test_ntfy_requires_safe_https_endpoint(self):
        for value in (
            "",
            "http://example.com",
            "https://key@example.com",
            "https://example.com?token=secret",
            "https://example.com/#fragment",
            "https://example.com\n",
            "https://example.com:invalid",
            "https://example.com:0",
            "https://example.com:70000",
        ):
            with self.subTest(value=value), self.assertRaises(watch.WatchError):
                watch.ntfy_endpoint(value)
        self.assertEqual(
            watch.ntfy_endpoint("https://example.com/ntfy/"),
            "https://example.com/ntfy/",
        )

    def test_ntfy_token_stays_in_header_and_requires_acknowledgement(self):
        self.write_token()
        acknowledgement = {
            "id": "message123",
            "event": "message",
            "topic": "telegravim-updates",
        }
        with patch.object(
            watch, "request_json", return_value=acknowledgement
        ) as request:
            watch.send_notification(self.config, "v7.2.5")
        args, kwargs = request.call_args
        self.assertEqual(args, ("https://ntfy.example.com/",))
        self.assertEqual(kwargs["headers"], {"Authorization": "Bearer test-secret"})
        self.assertEqual(kwargs["data"]["topic"], "telegravim-updates")
        self.assertNotIn("test-secret", json.dumps(kwargs["data"]))
        for reply in (
            {},
            {"id": 0},
            {"id": True},
            {"id": "123"},
            [],
            acknowledgement | {"topic": "other-project"},
            acknowledgement | {"event": "open"},
        ):
            with (
                self.subTest(reply=reply),
                patch.object(watch, "request_json", return_value=reply),
                self.assertRaises(watch.WatchError),
            ):
                watch.send_notification(self.config, "v7.2.5")

    def test_token_permissions_and_symlinks_are_rejected(self):
        self.write_token()
        token_path = self.config["ntfy_token_file"]
        token_path.chmod(0o644)
        with (
            patch.object(watch, "request_json") as request,
            self.assertRaises(watch.WatchError),
        ):
            watch.send_notification(self.config)
        request.assert_not_called()
        linked = self.root / "linked.token"
        linked.symlink_to(token_path)
        self.config["ntfy_token_file"] = linked
        with self.assertRaises(watch.WatchError):
            watch.send_notification(self.config)

    def test_http_errors_do_not_echo_remote_body_or_url(self):
        error = urllib.error.HTTPError(
            "https://secret.invalid", 401, "test-secret", {}, None
        )
        with patch.object(watch.urllib.request, "build_opener") as opener:
            opener.return_value.open.side_effect = error
            with self.assertRaisesRegex(
                watch.WatchError, "^HTTP request failed with status 401$"
            ):
                watch.request_json("https://example.com")

    def test_redirects_are_not_followed(self):
        self.assertIsNone(
            watch.NoRedirect().redirect_request(
                None, None, 302, "", {}, "https://another.example"
            )
        )

    def test_incomplete_http_response_is_sanitized(self):
        error = watch.http.client.IncompleteRead(b"test-secret")
        with patch.object(watch.urllib.request, "build_opener") as opener:
            opener.return_value.open.side_effect = error
            with self.assertRaisesRegex(watch.WatchError, "^Network request failed$"):
                watch.request_json(watch.LATEST_RELEASE)

    def test_http_timeout_and_response_size_are_bounded(self):
        response = Mock(status=200)
        response.read.return_value = b"x" * (watch.MAX_RESPONSE_BYTES + 1)
        with patch.object(watch.urllib.request, "build_opener") as opener:
            opener.return_value.open.return_value.__enter__.return_value = response
            with self.assertRaisesRegex(watch.WatchError, "HTTP response is too large"):
                watch.request_json(watch.LATEST_RELEASE)
            self.assertEqual(opener.return_value.open.call_args.kwargs["timeout"], 20)

    def test_test_notification_does_not_change_release_state(self):
        with (
            patch.object(watch, "send_notification") as send,
            patch.object(watch, "fetch_release") as fetch,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(
                watch.main(["--config", str(self.config_path), "--test-notification"]),
                0,
            )
            send.assert_called_once()
            fetch.assert_not_called()
        self.assertFalse(self.config["state_file"].exists())

    def test_cli_errors_have_nonzero_exit_and_no_secret(self):
        stderr = io.StringIO()
        with (
            patch.object(watch, "read_config", side_effect=OSError("test-secret")),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(watch.main(["--config", str(self.config_path)]), 1)
        self.assertNotIn("test-secret", stderr.getvalue())
        self.assertEqual(json.loads(stderr.getvalue())["status"], "error")


if __name__ == "__main__":
    unittest.main()
