"""Read the committed Android fork version independently of upstream and Desktop."""

import hashlib
import re


def read_release_version(android):
    path = android / "overlay/televim-version.properties"
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 1024:
        raise ValueError("Android fork version must be a bounded regular file")
    data = path.read_bytes()
    match = re.fullmatch(
        rb"TELEVIM_VERSION_CODE=([1-9][0-9]{0,8})\n"
        rb"TELEVIM_VERSION_NAME=([A-Za-z0-9][A-Za-z0-9.+_-]{0,63})\n",
        data,
    )
    if not match or int(match[1]) > 209999999:
        raise ValueError("Invalid committed Android fork version")
    base_code = int(match[1])
    return {
        "base_code": base_code,
        "version_name": match[2].decode("ascii"),
        "apk_code": base_code * 10 + 9,
        "source_sha256": hashlib.sha256(data).hexdigest(),
    }
