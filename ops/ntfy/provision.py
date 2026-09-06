#!/usr/bin/env python3
"""Provision private ntfy configuration without displaying credentials."""

import argparse
import datetime
import json
import os
import re
import secrets
import subprocess
import sys
from pathlib import Path

IMAGE = "binwiederhier/ntfy:v2.28.0@sha256:6ef4b819f722fccdc036af611c4774cfdc2de821ab74fdd48bbf4c9d6f8973da"
TOPIC = "telegravim-updates"


def private_write(path, content):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w") as target:
        target.write(content)


def ntfy(*args, stdin=None):
    result = subprocess.run(
        [
            "docker",
            "run",
            "--rm",
            "-i",
            "--network",
            "none",
            "--read-only",
            "--user",
            f"{os.getuid()}:{os.getgid()}",
            "--cap-drop",
            "ALL",
            "--security-opt",
            "no-new-privileges",
            IMAGE,
            *args,
        ],
        input=stdin,
        text=True,
        capture_output=True,
        check=False,
        timeout=90,
    )
    if result.returncode:
        raise ValueError("ntfy credential generation failed")
    return result.stdout.strip()


def reader_name(bootstrap):
    readers = [
        name
        for name, user in bootstrap["users"].items()
        if user["role"] == "user" and name != "telegravim-publisher"
    ]
    if len(readers) != 1:
        raise ValueError("Expected exactly one device reader in bootstrap")
    return readers[0]


def server_config(domain, bootstrap, contact):
    return {
        "base-url": "https://" + domain,
        "listen-http": ":8080",
        "cache-file": "/var/lib/ntfy/cache.db",
        "cache-duration": "168h",
        "auth-file": "/var/lib/ntfy/user.db",
        "auth-default-access": "deny-all",
        "auth-users": [
            f"{name}:{value['hash']}:{value['role']}"
            for name, value in bootstrap["users"].items()
        ],
        "auth-access": [
            f"{reader_name(bootstrap)}:{TOPIC}:ro",
            f"telegravim-publisher:{TOPIC}:wo",
        ],
        "auth-tokens": [
            f"telegravim-publisher:{bootstrap['publisher_token']}:Telegram release watcher"
        ],
        "enable-login": True,
        "require-login": True,
        "enable-signup": False,
        "enable-reservations": False,
        "behind-proxy": True,
        "upstream-base-url": "https://ntfy.sh",
        "web-push-public-key": bootstrap["vapid_public"],
        "web-push-private-key": bootstrap["vapid_private"],
        "web-push-file": "/var/lib/ntfy/webpush.db",
        "web-push-email-address": contact,
        "log-level": "info",
    }


def prepare(root, domain, contact):
    if not re.fullmatch(r"[a-z0-9]+(?:[.-][a-z0-9]+)+", domain):
        raise ValueError("Invalid domain")
    contact = contact or "https://" + domain
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    for name in (
        "secrets",
        "config",
        "data",
        "data/ntfy",
        "data/caddy",
        "data/caddy-config",
        "backups",
    ):
        (root / name).mkdir(mode=0o700, exist_ok=True)
    config_file = root / "config/server.yml"
    if config_file.exists():
        return {"status": "already_prepared", "existing_files": "preserved"}
    bootstrap_file = root / "secrets/bootstrap.json"
    if bootstrap_file.exists():
        bootstrap = json.loads(bootstrap_file.read_text())
    else:
        if (root / "data/ntfy/user.db").exists():
            raise ValueError("Existing database without bootstrap; recover manually")
        users = {}
        for name, role in (
            ("server-admin", "admin"),
            ("subscriber", "user"),
            ("telegravim-publisher", "user"),
        ):
            password = secrets.token_urlsafe(24)
            hashed = ntfy("user", "hash", stdin=password + "\n" + password + "\n")
            if not re.fullmatch(r"\$2[aby]\$\d\d\$[./A-Za-z0-9]{53}", hashed):
                raise ValueError("Invalid generated password hash")
            users[name] = {"password": password, "hash": hashed, "role": role}
        vapid_output = ntfy("webpush", "keys")
        keys = dict(
            re.findall(
                r"^(web-push-(?:public|private)-key): ([A-Za-z0-9_-]+)$",
                vapid_output,
                re.MULTILINE,
            )
        )
        token = ntfy("token", "generate")
        if not re.fullmatch(r"tk_[A-Za-z0-9]+", token):
            raise ValueError("Invalid generated token")
        bootstrap = {
            "users": users,
            "publisher_token": token,
            "vapid_public": keys["web-push-public-key"],
            "vapid_private": keys["web-push-private-key"],
        }
        private_write(bootstrap_file, json.dumps(bootstrap, indent=2) + "\n")
    reader = reader_name(bootstrap)
    for name, user in bootstrap["users"].items():
        path = root / "secrets" / (name + ".password")
        if not path.exists():
            private_write(path, user["password"] + "\n")
    env_file = root / ".env"
    if not env_file.exists():
        private_write(
            env_file,
            f"SERVICE_UID={os.getuid()}\nSERVICE_GID={os.getgid()}\nNOTIFY_DOMAIN={domain}\n",
        )
    private_write(
        config_file,
        json.dumps(server_config(domain, bootstrap, contact), indent=2) + "\n",
    )
    return {"status": "prepared", "domain": domain, "topic": TOPIC, "reader": reader}


def connect_watcher(root, directory):
    bootstrap = json.loads((root / "secrets/bootstrap.json").read_text())
    server = json.loads((root / "config/server.yml").read_text())
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    token_path = directory / "ntfy.token"
    if not token_path.exists():
        private_write(token_path, bootstrap["publisher_token"] + "\n")
    elif token_path.read_text().strip() != bootstrap["publisher_token"]:
        raise ValueError("Existing watcher token differs")
    config = {
        "baseline_version": "7.1.3",
        "ntfy_url": server["base-url"],
        "ntfy_topic": TOPIC,
        "ntfy_token_file": str(token_path),
        "state_file": str(
            Path.home() / ".local/state/telegravim-upstream-watch/state.json"
        ),
    }
    config_path = directory / "config.json"
    if not config_path.exists():
        private_write(config_path, json.dumps(config, indent=2) + "\n")
    elif json.loads(config_path.read_text()) != config:
        raise ValueError("Existing watcher config differs")
    return {"status": "watcher_configured", "topic": TOPIC}


def merged_crontab(current, template):
    lines = [
        line
        for line in template.splitlines()
        if line.strip() and not line.startswith("#")
    ]
    if len(lines) != 1 or "telegravim-upstream-watch" not in lines[0]:
        raise ValueError("Expected one watcher cron entry")
    existing = [
        line
        for line in current.splitlines()
        if "telegravim-upstream-watch" in line and not line.lstrip().startswith("#")
    ]
    if existing:
        if existing == lines:
            return current
        raise ValueError("Conflicting watcher cron entry; preserve and review manually")
    return (
        current
        + ("\n" if current and not current.endswith("\n") else "")
        + lines[0]
        + "\n"
    )


def schedule(root, directory):
    config = json.loads((directory / "config.json").read_text())
    state = json.loads(Path(config["state_file"]).read_text())
    if state.get("schema_version") != 1 or not state.get("notified_tag"):
        raise ValueError("Verify a real notification before enabling cron")
    environment = dict(os.environ, LC_ALL="C")
    current = subprocess.run(
        ["crontab", "-l"],
        capture_output=True,
        text=True,
        env=environment,
        timeout=10,
        check=False,
    )
    if current.returncode and not (
        current.returncode == 1 and current.stderr.startswith("no crontab for ")
    ):
        raise ValueError("Cannot read existing crontab")
    updated = merged_crontab(
        current.stdout, (root / "telegravim-upstream-watch.cron").read_text()
    )
    if updated == current.stdout:
        return {"status": "already_scheduled"}
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    private_write(root / "backups" / ("crontab-" + stamp), current.stdout)
    for args in (["crontab", "-n", "-"], ["crontab", "-"]):
        result = subprocess.run(
            args,
            input=updated,
            capture_output=True,
            check=False,
            text=True,
            env=environment,
            timeout=10,
        )
        if result.returncode:
            raise ValueError("Cron validation or installation failed")
    return {"status": "scheduled", "schedule": "17 */6 * * *"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["prepare", "connect-watcher", "schedule"])
    parser.add_argument("--root", type=Path, default=Path.home() / "services/ntfy")
    parser.add_argument("--domain")
    parser.add_argument("--contact", default="")
    parser.add_argument(
        "--watcher-config-dir",
        type=Path,
        default=Path.home() / ".config/telegravim-upstream-watch",
    )
    args = parser.parse_args()
    if args.action == "prepare" and not args.domain:
        parser.error("prepare requires --domain")
    try:
        if args.action == "prepare":
            result = prepare(args.root, args.domain, args.contact)
        elif args.action == "connect-watcher":
            result = connect_watcher(args.root, args.watcher_config_dir)
        else:
            result = schedule(args.root, args.watcher_config_dir)
        print(json.dumps(result))
        return 0
    except (OSError, ValueError, KeyError, subprocess.SubprocessError):
        print(
            "Provisioning failed; existing files preserved. Inspect config/state without exposing secrets.",
            file=sys.stderr,
        )
        return 1


if __name__ == "__main__":
    sys.exit(main())
