#!/usr/bin/env python3
"""Notify about stable Telegram releases; never clone, build, or run an agent."""

import argparse
import contextlib
import datetime
import fcntl
import http.client
import json
import os
import re
import stat
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

LATEST_RELEASE = "https://api.github.com/repos/telegramdesktop/tdesktop/releases/latest"
RELEASE_PAGE = "https://github.com/telegramdesktop/tdesktop/releases/tag/"
MAX_RESPONSE_BYTES = 131072


class WatchError(Exception):
    pass


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def version(value):
    if not isinstance(value, str) or not re.fullmatch(
        r"v?[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}", value
    ):
        raise WatchError("Expected a stable version in X.Y.Z format")
    return tuple(int(part) for part in value.removeprefix("v").split("."))


def read_json(path):
    try:
        with path.open("rb") as source:
            raw = source.read(MAX_RESPONSE_BYTES + 1)
        if len(raw) > MAX_RESPONSE_BYTES:
            raise WatchError("Local JSON file is too large")
        return json.loads(raw)
    except (ValueError, UnicodeError):
        raise WatchError("Invalid local JSON file") from None


def absolute_path(value):
    if not isinstance(value, str) or not value:
        raise WatchError("File paths must be nonempty strings")
    path = Path(value).expanduser()
    if not path.is_absolute():
        raise WatchError("File paths must be absolute or start with ~/")
    return path


def read_config(path):
    config = read_json(path)
    keys = {
        "baseline_version",
        "ntfy_url",
        "ntfy_topic",
        "ntfy_token_file",
        "state_file",
    }
    if not isinstance(config, dict) or set(config) != keys:
        raise WatchError(
            "Config requires baseline_version, ntfy_url, ntfy_topic, ntfy_token_file, state_file"
        )
    version(config["baseline_version"])
    for key in ("ntfy_token_file", "state_file"):
        config[key] = absolute_path(config[key])
    protected = {path.resolve(), config["ntfy_token_file"].resolve()}
    if config["state_file"].resolve() in protected:
        raise WatchError("Config, token, and state must use separate files")
    if not isinstance(config["ntfy_url"], str):
        raise WatchError("ntfy URL must be a string")
    if not isinstance(config["ntfy_topic"], str) or not re.fullmatch(
        r"[A-Za-z0-9_-]{1,64}", config["ntfy_topic"]
    ):
        raise WatchError(
            "ntfy topic must contain 1-64 letters, digits, underscores or hyphens"
        )
    return config


def request_json(url, data=None, headers=None):
    request_headers = {
        "User-Agent": "Telegravim-Upstream-Watch",
        "Accept": "application/json",
    }
    request_headers.update(headers or {})
    payload = None if data is None else json.dumps(data).encode("utf-8")
    if payload is not None:
        request_headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=payload, headers=request_headers)
    try:
        with urllib.request.build_opener(NoRedirect()).open(
            request, timeout=20
        ) as response:
            if not 200 <= response.status < 300:
                raise WatchError("Unexpected HTTP status")
            raw = response.read(MAX_RESPONSE_BYTES + 1)
        if len(raw) > MAX_RESPONSE_BYTES:
            raise WatchError("HTTP response is too large")
        return json.loads(raw)
    except urllib.error.HTTPError as error:
        code = error.code
        error.close()
        raise WatchError(f"HTTP request failed with status {code}") from None
    except (urllib.error.URLError, TimeoutError, OSError, http.client.HTTPException):
        raise WatchError("Network request failed") from None
    except (ValueError, UnicodeError):
        raise WatchError("Invalid JSON response") from None


def fetch_release():
    release = request_json(LATEST_RELEASE)
    if not isinstance(release, dict):
        raise WatchError("Invalid release response")
    if release.get("draft") is not False or release.get("prerelease") is not False:
        raise WatchError("Latest release is not a published stable release")
    tag = release.get("tag_name")
    version(tag)
    return tag


def ntfy_endpoint(value):
    try:
        url = urllib.parse.urlsplit(value)
        valid = (
            url.scheme == "https"
            and url.hostname
            and not url.username
            and not url.password
            and not url.query
            and not url.fragment
            and not any(char.isspace() for char in value)
        )
        if not valid:
            raise ValueError()
        if url.port is not None and not 1 <= url.port <= 65535:
            raise ValueError()
        return value.rstrip("/") + "/"
    except ValueError:
        raise WatchError(
            "ntfy requires an HTTPS base URL without credentials or query parameters"
        ) from None


def send_notification(config, tag=None):
    endpoint = ntfy_endpoint(config["ntfy_url"])
    token_path = config["ntfy_token_file"]
    info = token_path.lstat()
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_uid != os.getuid()
        or info.st_mode & 0o077
    ):
        raise WatchError(
            "ntfy token must be a private, user-owned regular file (chmod 600)"
        )
    with token_path.open(encoding="utf-8") as source:
        token = source.read(4097).strip()
    if (
        not token
        or len(token) > 4096
        or not token.isascii()
        or any(char.isspace() for char in token)
    ):
        raise WatchError("Invalid ntfy token file")
    title = f"Telegram Desktop {tag}" if tag else "Telegravim: notification test"
    message = (
        "New stable Telegram release. Run the Telegravim compatibility checks "
        "and Vim tests locally before building a new macOS DMG.\n" + RELEASE_PAGE + tag
        if tag
        else "Notification delivery works. No update, compatibility check, agent, or build was started."
    )
    result = request_json(
        endpoint,
        data={
            "topic": config["ntfy_topic"],
            "title": title,
            "message": message,
            "priority": 4,
        },
        headers={"Authorization": "Bearer " + token},
    )
    if (
        not isinstance(result, dict)
        or not isinstance(result.get("id"), str)
        or not result["id"]
        or result.get("event") != "message"
        or result.get("topic") != config["ntfy_topic"]
    ):
        raise WatchError(
            "ntfy did not acknowledge the notification in the expected topic"
        )


def threshold(config):
    baseline = version(config["baseline_version"])
    try:
        state = read_json(config["state_file"])
    except FileNotFoundError:
        return baseline
    if not isinstance(state, dict) or state.get("schema_version") != 1:
        raise WatchError("Invalid notification state; refusing to overwrite it")
    return max(baseline, version(state.get("notified_tag")))


def save_state(path, tag):
    state = {
        "schema_version": 1,
        "notified_tag": tag,
        "notified_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    }
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", dir=path.parent, delete=False
        ) as target:
            temporary = Path(target.name)
            json.dump(state, target)
            target.write("\n")
            target.flush()
            os.fsync(target.fileno())
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


@contextlib.contextmanager
def locked_state(path):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor = os.open(str(path) + ".lock", os.O_CREAT | os.O_RDWR, 0o600)
    with os.fdopen(descriptor, "w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            yield False
            return
        yield True


def check(config, dry_run=False):
    minimum = threshold(config)
    tag = fetch_release()
    if version(tag) <= minimum:
        return {"status": "no_update", "tag": tag}
    result = {"status": "update_available", "tag": tag, "url": RELEASE_PAGE + tag}
    if not dry_run:
        send_notification(config, tag)
        save_state(config["state_file"], tag)
        result["status"] = "notified"
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--dry-run",
        action="store_true",
        help="Check without secrets, notifications, or state changes",
    )
    mode.add_argument(
        "--test-notification",
        action="store_true",
        help="Send a test without changing release state",
    )
    args = parser.parse_args(argv)
    try:
        config = read_config(args.config)
        if args.dry_run:
            result = check(config, dry_run=True)
        elif args.test_notification:
            send_notification(config)
            result = {"status": "test_notified"}
        else:
            with locked_state(config["state_file"]) as acquired:
                result = check(config) if acquired else {"status": "already_running"}
        print(json.dumps(result))
        return 0
    except WatchError as error:
        print(json.dumps({"status": "error", "message": str(error)}), file=sys.stderr)
    except (OSError, UnicodeError):
        print(
            json.dumps({"status": "error", "message": "Local file operation failed"}),
            file=sys.stderr,
        )
    return 1


if __name__ == "__main__":
    sys.exit(main())
