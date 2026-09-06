"""Install fixed scanner releases, verifying their published SHA-256 digests."""

import hashlib
import io
import os
from pathlib import Path
import platform
import tarfile
import urllib.request

TOOLS = {
    "gitleaks": (
        "gitleaks/gitleaks",
        "v8.30.1",
        {
            "Linux": (
                "gitleaks_8.30.1_linux_x64.tar.gz",
                "551f6fc83ea457d62a0d98237cbad105af8d557003051f41f3e7ca7b3f2470eb",
            ),
            "Darwin": (
                "gitleaks_8.30.1_darwin_arm64.tar.gz",
                "b40ab0ae55c505963e365f271a8d3846efbc170aa17f2607f13df610a9aeb6a5",
            ),
        },
    ),
    "actionlint": (
        "rhysd/actionlint",
        "v1.7.12",
        {
            "Linux": (
                "actionlint_1.7.12_linux_amd64.tar.gz",
                "8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8",
            ),
            "Darwin": (
                "actionlint_1.7.12_darwin_arm64.tar.gz",
                "aba9ced2dee8d27fecca3dc7feb1a7f9a52caefa1eb46f3271ea66b6e0e6953f",
            ),
        },
    ),
    "zizmor": (
        "zizmorcore/zizmor",
        "v1.30.0",
        {
            "Linux": (
                "zizmor-x86_64-unknown-linux-gnu.tar.gz",
                "ec8c95cd800845abb9bbc5f377ec7c57d2eb8e2386a00a201d3a74ee4092e5ed",
            ),
            "Darwin": (
                "zizmor-aarch64-apple-darwin.tar.gz",
                "c9c5d83730efb86f2cd71b487605c00a4d63903e4f9458485ed5eac3b1924ab1",
            ),
        },
    ),
}


def install(destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for name, (repo, version, assets) in TOOLS.items():
        asset, expected = assets[platform.system()]
        url = f"https://github.com/{repo}/releases/download/{version}/{asset}"
        with urllib.request.urlopen(url, timeout=60) as response:
            archive = response.read(50_000_001)
        if hashlib.sha256(archive).hexdigest() != expected:
            raise RuntimeError(f"{name}: download checksum mismatch")
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as bundle:
            members = [m for m in bundle if m.isfile() and Path(m.name).name == name]
            if len(members) != 1:
                raise RuntimeError(f"{name}: unexpected archive contents")
            with bundle.extractfile(members[0]) as source:
                target = destination / name
                target.write_bytes(source.read())
                target.chmod(0o755)
        print(f"Installed {name} {version} (SHA-256 verified)")


if __name__ == "__main__":
    install(Path(os.environ["SCANNER_BIN"]))
