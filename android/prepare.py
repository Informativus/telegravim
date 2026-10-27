#!/usr/bin/env python3
"""Prepare pinned Android sources and run a locked build without editing upstream."""

import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile


SCHEMA = 1
METADATA = ".televim-source.json"
GENERATED = (".gradle", ".kotlin", "buildSrc/.gradle", "buildSrc/.kotlin")
SHA1 = re.compile(r"[0-9a-f]{40}")
SHA256 = re.compile(r"[0-9a-f]{64}")


class PreparationError(Exception):
    pass


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def publish_metadata(path, data):
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise PreparationError("Build metadata must be a regular file")
    descriptor, temporary = tempfile.mkstemp(prefix=".source-metadata-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def git_environment():
    environment = os.environ.copy()
    environment["GIT_NO_REPLACE_OBJECTS"] = "1"
    environment["GIT_OPTIONAL_LOCKS"] = "0"
    environment["GIT_CONFIG_NOSYSTEM"] = "1"
    environment["GIT_CONFIG_GLOBAL"] = os.devnull
    environment["GIT_ATTR_NOSYSTEM"] = "1"
    for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES"):
        environment.pop(key, None)
    return environment


def git(repository, *arguments, allowed=(0,), environment=None):
    result = subprocess.run(["git", "-C", str(repository), *arguments],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=environment or git_environment())
    if result.returncode not in allowed:
        raise PreparationError("Git operation failed: " + " ".join(arguments[:2]))
    if len(result.stdout) > 64 * 1024 * 1024:
        raise PreparationError("Git metadata exceeds the preparation limit")
    return result


def relative_path(value):
    if not isinstance(value, str) or not value or "\\" in value or any(ord(c) < 32 for c in value):
        raise PreparationError("Invalid source path")
    path = PurePosixPath(value)
    if path.is_absolute() or path.as_posix() != value or any(part in (".", "..") or part.lower() == ".git" for part in path.parts):
        raise PreparationError("Unsafe source path")
    if value == METADATA or any(value == prefix or value.startswith(prefix + "/") for prefix in GENERATED):
        raise PreparationError("A source input uses a reserved generated path")
    return value


def safe_path(root, name, include_leaf=False):
    if root.is_symlink():
        raise PreparationError("A source input directory must not be a symlink")
    name = relative_path(name)
    destination = root.joinpath(*PurePosixPath(name).parts)
    current = root
    parts = PurePosixPath(name).parts if include_leaf else PurePosixPath(name).parts[:-1]
    for part in parts:
        current = current / part
        if current.is_symlink():
            raise PreparationError("A source path traverses a symlink: " + name)
    return destination


def load_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise PreparationError("Duplicate JSON metadata key")
            result[key] = value
        return result

    if path.stat().st_size > 64 * 1024 * 1024:
        raise PreparationError("Preparation metadata exceeds its limit")
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)


def load_inputs(android):
    manifest_path = android / "source.json"
    if manifest_path.is_symlink():
        raise PreparationError("Source manifest must be a regular file")
    manifest = load_json(manifest_path)
    if not isinstance(manifest, dict) or set(manifest) != {"upstream_commit", "overlay", "patches", "omit"}:
        raise PreparationError("Unsupported Android source manifest")
    if not isinstance(manifest["upstream_commit"], str) or not SHA1.fullmatch(manifest["upstream_commit"]):
        raise PreparationError("Unsupported Android source manifest")
    if not isinstance(manifest["overlay"], dict) or not isinstance(manifest["patches"], dict) or not isinstance(manifest["omit"], list):
        raise PreparationError("Invalid Android source manifest collections")
    inputs = {}
    modes = {}
    for kind in ("overlay", "patches"):
        inputs[kind] = {}
        modes[kind] = {}
        for name, expected in sorted(manifest[kind].items()):
            relative_path(name)
            if kind == "patches" and ("/" in name or not name.endswith(".patch")):
                raise PreparationError("Patch names must be plain .patch filenames")
            if not isinstance(expected, str) or not SHA256.fullmatch(expected):
                raise PreparationError("Invalid input SHA-256")
            path = safe_path(android / kind, name, include_leaf=True)
            info = path.stat()
            if not stat.S_ISREG(info.st_mode) or info.st_size > 16 * 1024 * 1024:
                raise PreparationError("Input must be a bounded regular file: " + name)
            data = path.read_bytes()
            if digest(data) != expected:
                raise PreparationError("Input checksum mismatch: " + kind + "/" + name)
            inputs[kind][name] = data
            modes[kind][name] = 0o755 if info.st_mode & 0o111 else 0o644
    omitted = [relative_path(name) for name in manifest["omit"]]
    if len(set(omitted)) != len(omitted):
        raise PreparationError("Duplicate omitted source path")
    return manifest, inputs, modes


def verify_repositories(upstream, expected):
    repositories = []
    dependencies = {}

    def visit(path, commit, prefix):
        if len(repositories) >= 256:
            raise PreparationError("Too many recursive source dependencies")
        if not path.is_dir() or path.is_symlink():
            raise PreparationError("Initialize the pinned submodule: " + (prefix or "android/upstream"))
        top = Path(git(path, "rev-parse", "--show-toplevel").stdout.decode().strip()).resolve()
        if top != path.resolve():
            raise PreparationError("Missing independent submodule checkout: " + (prefix or "android/upstream"))
        head = git(path, "rev-parse", "HEAD").stdout.decode().strip()
        if head != commit:
            raise PreparationError("Submodule is not at its pinned commit: " + (prefix or "android/upstream"))
        for arguments in (("diff", "--quiet", "--ignore-submodules=all", "--"),
                          ("diff", "--quiet", "--cached", "--ignore-submodules=all", "HEAD", "--")):
            if git(path, *arguments, allowed=(0, 1)).returncode:
                raise PreparationError("Pinned source has tracked modifications: " + (prefix or "android/upstream"))
        repositories.append((path, commit, prefix))
        tree = git(path, "ls-tree", "-rz", "--full-tree", commit).stdout
        for entry in tree.split(b"\0"):
            if not entry:
                continue
            header, raw_name = entry.split(b"\t", 1)
            mode, kind, sha = header.decode("ascii").split()
            if mode != "160000":
                continue
            name = relative_path(raw_name.decode("utf-8"))
            full = relative_path(prefix + "/" + name if prefix else name)
            if kind != "commit" or not SHA1.fullmatch(sha) or full in dependencies:
                raise PreparationError("Invalid recursive gitlink")
            dependencies[full] = sha
            visit(safe_path(upstream, full, include_leaf=True), sha, full)

    visit(upstream, expected, "")
    return repositories, dict(sorted(dependencies.items()))


def archive(repository, commit, prefix, destination):
    command = ["git", "-C", str(repository), "archive", "--format=tar"]
    if prefix:
        command.append("--prefix=" + prefix + "/")
    command.append(commit)
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors, env=git_environment())
        try:
            with tarfile.open(fileobj=process.stdout, mode="r|") as stream:
                for entry in stream:
                    name = entry.name.rstrip("/")
                    if not name:
                        continue
                    path = safe_path(destination, name)
                    if entry.isdir():
                        if path.is_symlink() or (path.exists() and not path.is_dir()):
                            raise PreparationError("Archive directory collision: " + name)
                        path.mkdir(parents=True, exist_ok=True)
                    elif entry.isreg():
                        path.parent.mkdir(parents=True, exist_ok=True)
                        if shutil.disk_usage(destination).free < entry.size + 256 * 1024 * 1024:
                            raise PreparationError("Insufficient free space for Android source preparation")
                        source = stream.extractfile(entry)
                        if source is None:
                            raise PreparationError("Missing archive content")
                        with source, path.open("xb") as target:
                            shutil.copyfileobj(source, target, 1024 * 1024)
                        path.chmod(0o755 if entry.mode & 0o111 else 0o644)
                    elif entry.issym():
                        target = entry.linkname
                        if os.path.isabs(target) or "\\" in target or "\0" in target:
                            raise PreparationError("Unsafe archived symlink")
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.symlink_to(target)
                    else:
                        raise PreparationError("Unsupported archive member: " + name)
            if process.wait() != 0:
                raise PreparationError("Could not archive a pinned source tree")
        except BaseException:
            process.terminate()
            process.wait()
            raise
        finally:
            process.stdout.close()


def inventory(root):
    entries = {}
    for directory, directories, files in os.walk(root, followlinks=False):
        parent = Path(directory)
        for name in sorted(directories + files):
            path = parent / name
            relative = path.relative_to(root).as_posix()
            if relative == METADATA or relative in GENERATED:
                if path.is_symlink():
                    raise PreparationError("Generated cache path must not be a symlink")
                if name in directories:
                    directories.remove(name)
                continue
            relative_path(relative)
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode):
                try:
                    path.resolve().relative_to(root.resolve())
                except (ValueError, RuntimeError):
                    raise PreparationError("Prepared source contains an escaping symlink: " + relative)
                entries[relative] = {"type": "symlink", "target": os.readlink(path)}
                if name in directories:
                    directories.remove(name)
            elif stat.S_ISREG(info.st_mode):
                entries[relative] = {"type": "file", "sha256": file_digest(path), "executable": bool(info.st_mode & 0o111)}
            elif not stat.S_ISDIR(info.st_mode):
                raise PreparationError("Prepared source contains a special file: " + relative)
    return dict(sorted(entries.items()))


def customize(destination, manifest, inputs, modes, cache):
    environment = git_environment()
    environment["GIT_CEILING_DIRECTORIES"] = str(cache)
    with tempfile.TemporaryDirectory(prefix=".patches-", dir=cache) as temporary:
        for name, data in inputs["patches"].items():
            patch = Path(temporary) / name
            patch.write_bytes(data)
            statistics = git(destination, "apply", "--numstat", "-z", "--", str(patch), environment=environment).stdout
            for entry in statistics.split(b"\0"):
                if entry:
                    path = entry.split(b"\t", 2)[2].decode("utf-8")
                    safe_path(destination, path, include_leaf=True)
            git(destination, "apply", "--check", "--whitespace=error-all", "--", str(patch), environment=environment)
            git(destination, "apply", "--whitespace=error-all", "--", str(patch), environment=environment)
    for name, data in inputs["overlay"].items():
        path = safe_path(destination, name, include_leaf=True)
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists() and not path.is_file():
            raise PreparationError("Overlay target is not a file: " + name)
        path.write_bytes(data)
        path.chmod(modes["overlay"][name])
    for name in manifest["omit"]:
        path = safe_path(destination, name, include_leaf=True)
        if path.exists():
            if not path.is_file():
                raise PreparationError("Omitted credential path is not a regular file: " + name)
            path.unlink()
    for name in GENERATED:
        if (destination / name).exists():
            raise PreparationError("Pinned source unexpectedly occupies a generated cache path")


def source_state(repository, android, manifest):
    commit = git(repository, "rev-parse", "HEAD").stdout.decode().strip()
    dirty = any(git(repository, *arguments, allowed=(0, 1)).returncode for arguments in (
        ("diff", "--quiet", "--ignore-submodules=all", "--"),
        ("diff", "--quiet", "--cached", "--ignore-submodules=all", "HEAD", "--")))
    gitlink = git(repository, "ls-tree", "-z", "HEAD", "--", "android/upstream").stdout
    expected = ("160000 commit " + manifest["upstream_commit"] + "\tandroid/upstream\0").encode("ascii")
    if gitlink != expected:
        dirty = True
    paths = ["source.json", "prepare.py", "build.sh"]
    paths += [kind + "/" + name for kind in ("overlay", "patches") for name in manifest[kind]]
    for name in paths:
        path = android / name
        tracked = git(repository, "show", "HEAD:android/" + name, allowed=(0, 128))
        if tracked.returncode or tracked.stdout != path.read_bytes():
            dirty = True
    return commit, bool(dirty)


def verify_cache(path, expected):
    metadata_path = path / METADATA
    if path.is_symlink() or metadata_path.is_symlink() or not metadata_path.is_file():
        raise PreparationError("Prepared source cache has no valid inventory")
    metadata = load_json(metadata_path)
    if metadata.get("identity") != expected:
        raise PreparationError("Prepared source cache identity differs from the requested inputs")
    current = inventory(path)
    if current != metadata.get("inventory") or digest(canonical(current)) != metadata.get("inventory_sha256"):
        old = metadata.get("inventory", {})
        changed = sorted(key for key in set(current) | set(old) if current.get(key) != old.get(key))
        raise PreparationError("Prepared source was modified; refusing reuse. Changed paths: " + ", ".join(changed[:8]))
    return metadata


def prepare(repository, android, primary, release):
    manifest, inputs, modes = load_inputs(android)
    repositories, dependencies = verify_repositories(android / "upstream", manifest["upstream_commit"])
    commit, dirty = source_state(repository, android, manifest)
    if release and dirty:
        raise PreparationError("A release requires committed, unchanged monorepo sources and packaging inputs")
    overlay_identity = {"overlay": manifest["overlay"], "patches": manifest["patches"], "omit": manifest["omit"], "modes": modes}
    identity = {"schema_version": SCHEMA, "monorepo_commit": commit, "upstream_commit": manifest["upstream_commit"],
                "dependencies": dependencies, "manifest": manifest, "modes": modes,
                "helpers": {name: file_digest(android / name) for name in ("prepare.py", "build.sh")}}
    input_digest = digest(canonical(identity))
    cache = primary / ".local" / "android-source"
    cache.mkdir(parents=True, exist_ok=True)
    if cache.is_symlink():
        raise PreparationError("Android source cache must not be a symlink")
    destination = cache / input_digest
    if destination.exists():
        metadata = verify_cache(destination, identity)
    else:
        print("Preparing pinned Android source trees (" + str(len(dependencies)) + " dependencies).", file=sys.stderr)
        with tempfile.TemporaryDirectory(prefix=".preparing-", dir=cache) as temporary:
            stage = Path(temporary) / "source"
            stage.mkdir()
            for path, revision, prefix in repositories:
                archive(path, revision, prefix, stage)
            customize(stage, manifest, inputs, modes, cache)
            files = inventory(stage)
            metadata = {"identity": identity, "inventory": files, "inventory_sha256": digest(canonical(files))}
            (stage / METADATA).write_bytes(canonical(metadata) + b"\n")
            os.replace(stage, destination)
    summary = {"schema_version": SCHEMA, "monorepo_commit": commit, "monorepo_dirty": dirty,
               "upstream_commit": manifest["upstream_commit"], "dependencies": dependencies,
               "input_digest": input_digest, "overlay_digest": digest(canonical(overlay_identity)),
               "source_inventory_sha256": metadata["inventory_sha256"]}
    return destination, identity, summary


@contextlib.contextmanager
def build_lock(primary):
    local = primary / ".local"
    local.mkdir(exist_ok=True)
    if local.is_symlink():
        raise PreparationError("The primary local directory must not be a symlink")
    descriptor = os.open(local / "android-build.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode) or os.fstat(descriptor).st_uid != os.getuid():
            raise PreparationError("Android build lock has an unexpected owner or type")
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise PreparationError("Another Android preparation or build holds the shared lock")
        yield descriptor
    finally:
        os.close(descriptor)


def build_environment(primary, repository, summary):
    environment = os.environ.copy()
    environment["TELEVIM_PRIMARY_ROOT"] = str(primary)
    environment["TELEVIM_MONOREPO_ROOT"] = str(repository)
    environment["TELEVIM_SOURCE_COMMIT"] = summary["monorepo_commit"]
    environment["TELEVIM_SOURCE_DIGEST"] = summary["input_digest"]
    output = primary / ".local" / "android-build"
    environment["TELEVIM_BUILD_ROOT"] = str(output)
    java_file = primary / ".local" / "private" / "android-toolchain" / "java-home.txt"
    if java_file.exists():
        if java_file.is_symlink() or java_file.stat().st_size > 4096:
            raise PreparationError("Invalid local Java home record")
        value = java_file.read_text(encoding="utf-8").rstrip("\r\n")
        if not value or "\n" in value or "\r" in value or not Path(value).is_absolute() or not (Path(value) / "bin" / "java").is_file():
            raise PreparationError("Local java-home.txt does not identify a valid Java installation")
        environment["JAVA_HOME"] = value
    if environment.get("JAVA_HOME"):
        environment["PATH"] = str(Path(environment["JAVA_HOME"]) / "bin") + os.pathsep + environment.get("PATH", "")
    sdk = primary / ".local" / "private" / "android-toolchain" / "sdk"
    if not environment.get("ANDROID_SDK_ROOT") and not environment.get("ANDROID_HOME") and sdk.is_dir():
        environment["ANDROID_SDK_ROOT"] = str(sdk)
    return environment, output


def run_build(source, variant, abis, environment, lock):
    child = subprocess.Popen(["bash", str(source / "scripts" / "build.sh"), variant, abis],
                             cwd=source, env=environment, pass_fds=(lock,), start_new_session=True)

    def interrupted(signum, frame):
        raise KeyboardInterrupt

    previous = signal.signal(signal.SIGTERM, interrupted)
    try:
        return child.wait()
    except KeyboardInterrupt:
        os.killpg(child.pid, signal.SIGTERM)
        try:
            child.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait()
        raise
    finally:
        signal.signal(signal.SIGTERM, previous)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", choices=("debug", "release"))
    parser.add_argument("--abis", default="arm64-v8a")
    arguments = parser.parse_args()
    abis = arguments.abis.split(",")
    if len(set(abis)) != len(abis) or not set(abis) <= {"armeabi-v7a", "arm64-v8a", "x86", "x86_64"}:
        raise PreparationError("Unsupported or duplicate Android ABI")
    android = Path(__file__).resolve().parent
    repository = android.parent
    common = Path(git(repository, "rev-parse", "--path-format=absolute", "--git-common-dir").stdout.decode().strip()).resolve()
    primary = common.parent
    if common.name != ".git":
        raise PreparationError("Could not identify the primary monorepo checkout")
    with build_lock(primary) as lock:
        source, identity, summary = prepare(repository, android, primary, arguments.build == "release")
        environment, output = build_environment(primary, repository, summary)
        output.mkdir(parents=True, exist_ok=True)
        if output.is_symlink():
            raise PreparationError("Android build output must not be a symlink")
        metadata_file = output / "source-metadata.json"
        publish_metadata(metadata_file, canonical(summary) + b"\n")
        print(source, flush=True)
        if arguments.build:
            result = run_build(source, arguments.build, arguments.abis, environment, lock)
            metadata = verify_cache(source, identity)
            if metadata["inventory_sha256"] != summary["source_inventory_sha256"]:
                raise PreparationError("Prepared source inventory changed during the Android build")
            current_manifest, _, _ = load_inputs(android)
            current_commit, current_dirty = source_state(repository, android, current_manifest)
            if current_manifest != identity["manifest"] or current_commit != summary["monorepo_commit"] or (arguments.build == "release" and current_dirty):
                raise PreparationError("Monorepo source changed during the Android build")
            return result
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("Android preparation/build interrupted.", file=sys.stderr)
        sys.exit(130)
    except (PreparationError, OSError, ValueError, tarfile.TarError) as error:
        print("Android preparation failed: " + str(error), file=sys.stderr)
        sys.exit(1)
