import argparse
import base64
import json
import os
from pathlib import Path
import re
import socket
import stat
import subprocess
import sys
import tempfile
import time


SOL_LOCAL = 0
LOCAL_PEERPID = 2


def run(app):
    profile = Path(tempfile.mkdtemp(prefix="telegravim-security-empty-profile-"))
    result = {"app": str(app), "profile": str(profile)}
    with (profile / "process-output.log").open("wb") as output:
        process = subprocess.Popen(
            [str(app / "Contents/MacOS/Telegram"), "-workdir", str(profile),
             "-noupdate", "-startintray"],
            stdout=output,
            stderr=subprocess.STDOUT,
        )
        result["pid"] = process.pid
        control = None
        try:
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"Exited before startup: {process.returncode}")
                text = "\n".join(
                    p.read_text(errors="replace") for p in profile.glob("log*.txt")
                )
                names = re.findall(r"Connecting local socket to (.+?)\.\.\.", text)
                if not names or not Path(names[-1]).exists():
                    time.sleep(0.3)
                    continue
                path = Path(names[-1])
                identity = path.stat()
                if identity.st_uid != os.geteuid() or not stat.S_ISSOCK(identity.st_mode):
                    raise RuntimeError("Unexpected socket identity")
                control = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                control.settimeout(3)
                control.connect(str(path))
                if control.getsockopt(SOL_LOCAL, LOCAL_PEERPID) != process.pid:
                    raise RuntimeError("Socket peer PID does not match the test app")
                control.sendall(b"CTRL:ping;")
                response = b""
                while b";" not in response:
                    chunk = control.recv(8192)
                    if not chunk or len(response) + len(chunk) > 16384:
                        raise RuntimeError("Missing or oversized local response")
                    response += chunk
                if b"DATA:" not in response:
                    raise RuntimeError("Missing automation response")
                encoded = response.split(b"DATA:", 1)[1].split(b";", 1)[0]
                payload = json.loads(base64.b64decode(encoded, validate=True))
                error = str(payload.get("error", ""))
                if "not launched" in error:
                    control.close()
                    control = None
                    time.sleep(0.3)
                    continue
                if "local automation is disabled" not in error:
                    raise RuntimeError("Fresh profile did not reject automation")
                result.update({
                    "startup": "PASS",
                    "automation_disabled": "PASS",
                    "socket_peer_pid_matches": True,
                    "socket_uid_matches": True,
                    "socket_mode": oct(stat.S_IMODE(identity.st_mode)),
                })
                break
            else:
                raise RuntimeError("Startup timed out")
            time.sleep(2)
            if process.poll() is not None:
                raise RuntimeError("App exited after startup")
            control.sendall(b"CMD:quit;")
            process.wait(timeout=12)
            result["clean_exit"] = process.returncode
            if process.returncode != 0:
                raise RuntimeError(f"Local quit failed: {process.returncode}")
        except Exception as error:
            result["error"] = str(error)
        finally:
            if control:
                control.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
    print(json.dumps(result, indent=2))
    return 1 if "error" in result else 0


def main():
    parser = argparse.ArgumentParser(
        description="Check macOS local control with a fresh temporary profile."
    )
    parser.add_argument("app", type=Path)
    args = parser.parse_args()
    app = args.app.resolve()
    if sys.platform != "darwin":
        parser.error("This smoke test requires macOS kernel peer PID support.")
    if not (app / "Contents/MacOS/Telegram").is_file():
        parser.error("Pass a built Telegram.app bundle.")
    return run(app)


if __name__ == "__main__":
    sys.exit(main())
