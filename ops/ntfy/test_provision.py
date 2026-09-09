import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

import provision


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.service = self.root / "service"
        self.watcher = self.root / "watcher"
        self.generated = [
            "$2a$10$" + "a" * 53,
            "$2a$10$" + "b" * 53,
            "$2a$10$" + "c" * 53,
            "web-push-public-key: public\nweb-push-private-key: private",
            "tk_test",
        ]

    def prepare(self):
        with patch.object(provision, "ntfy", side_effect=self.generated):
            return provision.prepare(self.service, "notify.example.com", "")

    def test_credentials_are_private_and_roles_are_least_privilege(self):
        result = self.prepare()
        self.assertEqual(result["status"], "prepared")
        config = json.loads((self.service / "config/server.yml").read_text())
        self.assertEqual(config["auth-default-access"], "deny-all")
        self.assertTrue(config["require-login"])
        self.assertFalse(config["enable-signup"])
        self.assertEqual(
            config["auth-access"],
            [
                "subscriber:telegravim-updates:ro",
                "telegravim-publisher:telegravim-updates:wo",
            ],
        )
        self.assertEqual(
            config["auth-tokens"],
            ["telegravim-publisher:tk_test:Telegram release watcher"],
        )
        self.assertEqual(config["web-push-email-address"], "https://notify.example.com")
        self.assertEqual(config["upstream-base-url"], "https://ntfy.sh")
        for path in self.service.rglob("*"):
            self.assertEqual(
                path.stat().st_mode & 0o777, 0o700 if path.is_dir() else 0o600
            )
        self.assertNotIn("tk_test", json.dumps(result))

    def test_repeat_preserves_existing_config_and_credentials(self):
        self.prepare()
        before = {
            path: path.read_bytes()
            for path in self.service.rglob("*")
            if path.is_file()
        }
        with patch.object(provision, "ntfy") as generate:
            result = provision.prepare(self.service, "notify.example.com", "")
        generate.assert_not_called()
        self.assertEqual(result["status"], "already_prepared")
        self.assertEqual(before, {path: path.read_bytes() for path in before})

    def test_existing_database_without_bootstrap_is_not_reinitialized(self):
        database = self.service / "data/ntfy/user.db"
        database.parent.mkdir(parents=True)
        database.write_bytes(b"existing database")
        with patch.object(provision, "ntfy") as generate, self.assertRaises(ValueError):
            provision.prepare(self.service, "notify.example.com", "")
        generate.assert_not_called()
        self.assertEqual(database.read_bytes(), b"existing database")

    def test_recovery_preserves_existing_reader_and_credentials(self):
        self.prepare()
        bootstrap_path = self.service / "secrets/bootstrap.json"
        bootstrap = json.loads(bootstrap_path.read_text())
        bootstrap["users"]["existing-reader"] = bootstrap["users"].pop("subscriber")
        bootstrap_path.write_text(json.dumps(bootstrap))
        password = self.service / "secrets/subscriber.password"
        password.rename(self.service / "secrets/existing-reader.password")
        config_path = self.service / "config/server.yml"
        config_path.unlink()
        with patch.object(provision, "ntfy") as generate:
            result = provision.prepare(self.service, "notify.example.com", "")
        generate.assert_not_called()
        self.assertEqual(result["reader"], "existing-reader")
        config = json.loads(config_path.read_text())
        self.assertEqual(
            config["auth-access"],
            [
                "existing-reader:telegravim-updates:ro",
                "telegravim-publisher:telegravim-updates:wo",
            ],
        )
        self.assertEqual(json.loads(bootstrap_path.read_text()), bootstrap)
        self.assertFalse(password.exists())

    def test_recovery_rejects_ambiguous_reader_permissions(self):
        self.prepare()
        bootstrap_path = self.service / "secrets/bootstrap.json"
        bootstrap = json.loads(bootstrap_path.read_text())
        bootstrap["users"]["extra-reader"] = bootstrap["users"]["subscriber"]
        bootstrap_path.write_text(json.dumps(bootstrap))
        config_path = self.service / "config/server.yml"
        config_path.unlink()
        with patch.object(provision, "ntfy") as generate, self.assertRaises(ValueError):
            provision.prepare(self.service, "notify.example.com", "")
        generate.assert_not_called()
        self.assertFalse(config_path.exists())

    def test_prepare_requires_explicit_domain_before_creating_files(self):
        with (
            patch.object(
                provision.sys,
                "argv",
                ["provision.py", "prepare", "--root", str(self.service)],
            ),
            patch.object(provision.sys, "stderr", new_callable=io.StringIO),
            patch.object(provision, "ntfy") as generate,
            self.assertRaises(SystemExit) as error,
        ):
            provision.main()
        self.assertEqual(error.exception.code, 2)
        generate.assert_not_called()
        self.assertFalse(self.service.exists())

    def test_failed_generation_can_retry_without_partial_config(self):
        with (
            patch.object(provision, "ntfy", return_value="invalid"),
            self.assertRaises(ValueError),
        ):
            provision.prepare(self.service, "notify.example.com", "")
        self.assertFalse((self.service / "config/server.yml").exists())
        self.assertEqual(self.prepare()["status"], "prepared")

    def test_exclusive_write_does_not_follow_symlinks_or_overwrite(self):
        target = self.root / "target"
        provision.private_write(target, "original")
        link = self.root / "link"
        link.symlink_to(target)
        for path in (target, link):
            with self.assertRaises(FileExistsError):
                provision.private_write(path, "replacement")
        self.assertEqual(target.read_text(), "original")

    def test_watcher_connection_is_idempotent_and_preserves_conflicts(self):
        self.prepare()
        provision.connect_watcher(self.service, self.watcher)
        provision.connect_watcher(self.service, self.watcher)
        token = self.watcher / "ntfy.token"
        self.assertEqual(token.stat().st_mode & 0o777, 0o600)
        self.assertEqual(token.read_text(), "tk_test\n")
        token.write_text("different")
        with self.assertRaises(ValueError):
            provision.connect_watcher(self.service, self.watcher)
        self.assertEqual(token.read_text(), "different")

    def test_cron_preserves_unrelated_entries_and_is_idempotent(self):
        current = "MAILTO=operator@example.com\n5 * * * * /usr/bin/true"
        template = "# comment\n17 */6 * * * telegravim-upstream-watch\n"
        merged = provision.merged_crontab(current, template)
        self.assertTrue(merged.startswith(current + "\n"))
        self.assertEqual(merged.count("telegravim-upstream-watch"), 1)
        self.assertEqual(provision.merged_crontab(merged, template), merged)
        for conflict in (merged + template, "0 * * * * telegravim-upstream-watch\n"):
            with self.assertRaises(ValueError):
                provision.merged_crontab(conflict, template)

    def test_schedule_installs_once_and_rejects_crontab_read_errors(self):
        (self.service / "backups").mkdir(parents=True)
        self.watcher.mkdir()
        state = self.root / "state.json"
        state.write_text(json.dumps({"schema_version": 1, "notified_tag": "v7.2.5"}))
        (self.watcher / "config.json").write_text(
            json.dumps({"state_file": str(state)})
        )
        template = "17 */6 * * * telegravim-upstream-watch\n"
        (self.service / "telegravim-upstream-watch.cron").write_text(template)
        with patch.object(
            provision.subprocess,
            "run",
            side_effect=[
                Mock(returncode=1, stdout="", stderr="no crontab for test\n"),
                Mock(returncode=0),
                Mock(returncode=0),
            ],
        ) as run:
            self.assertEqual(
                provision.schedule(self.service, self.watcher)["status"], "scheduled"
            )
        self.assertEqual(run.call_args_list[1].args[0], ["crontab", "-n", "-"])
        self.assertEqual(run.call_args_list[2].args[0], ["crontab", "-"])
        self.assertEqual(run.call_args_list[2].kwargs["input"], template)
        with patch.object(
            provision.subprocess,
            "run",
            return_value=Mock(returncode=0, stdout=template),
        ) as run:
            self.assertEqual(
                provision.schedule(self.service, self.watcher)["status"],
                "already_scheduled",
            )
        self.assertEqual(run.call_count, 1)
        with (
            patch.object(
                provision.subprocess,
                "run",
                return_value=Mock(returncode=1, stdout="", stderr="permission denied"),
            ) as run,
            self.assertRaises(ValueError),
        ):
            provision.schedule(self.service, self.watcher)
        self.assertEqual(run.call_count, 1)
        self.assertEqual(len(list((self.service / "backups").iterdir())), 1)

    def test_schedule_requires_notified_state_and_backs_up_before_install(self):
        self.prepare()
        provision.connect_watcher(self.service, self.watcher)
        config_path = self.watcher / "config.json"
        config = json.loads(config_path.read_text())
        state_path = self.root / "state.json"
        config["state_file"] = str(state_path)
        config_path.write_text(json.dumps(config))
        state_path.write_text(json.dumps({"schema_version": 1}))
        with (
            patch.object(provision.subprocess, "run") as run,
            self.assertRaises(ValueError),
        ):
            provision.schedule(self.service, self.watcher)
        run.assert_not_called()
        state_path.write_text(
            json.dumps({"schema_version": 1, "notified_tag": "v7.2.5"})
        )
        (self.service / "telegravim-upstream-watch.cron").write_text(
            "17 */6 * * * telegravim-upstream-watch\n"
        )
        with (
            patch.object(
                provision.subprocess,
                "run",
                side_effect=[
                    Mock(returncode=0, stdout="5 * * * * /usr/bin/true\n"),
                    Mock(returncode=1),
                ],
            ) as run,
            self.assertRaises(ValueError),
        ):
            provision.schedule(self.service, self.watcher)
        self.assertEqual(run.call_count, 2)
        backups = list((self.service / "backups").iterdir())
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_text(), "5 * * * * /usr/bin/true\n")
        self.assertEqual(backups[0].stat().st_mode & 0o777, 0o600)


if __name__ == "__main__":
    unittest.main()
