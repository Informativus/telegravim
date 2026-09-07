"""Export an attributed wordfreq subset validated by the bundled Hunspell."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

import msgpack


REVISION = "912caf64b657478d1dff1138efdc078947d54bb1"
SOURCE_SHA256 = "0440613cc765c14a20fb9483225f2fec4d85e66672809076bcec9b29119f9b43"
NOTICE_SHA256 = "d9b104dafe63886af55e7cea7484824e32e12d4e1f57f319923dd643b36d6b15"
LICENSE = "https://creativecommons.org/licenses/by-sa/4.0/"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("notice", type=Path)
    parser.add_argument("validator", type=Path)
    args = parser.parse_args()
    resources = Path(__file__).resolve().parents[1] / "Resources/dictionaries"
    source = args.source.read_bytes()
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA256:
        raise ValueError("Unexpected wordfreq source revision")
    data = msgpack.unpackb(gzip.decompress(source), raw=False)
    if data[0] != {"format": "cB", "version": 1}:
        raise ValueError("Unexpected wordfreq format")
    rows = [
        (word, 900 - index)
        for index, words in enumerate(data[1:])
        if 900 - index >= 300
        for word in words
        if re.fullmatch("[а-яё]{1,24}", word)
    ]
    raw = "".join(f"{word}\t{frequency}\n" for word, frequency in rows)
    dictionary = b"".join(
        (resources / part).read_bytes() for part in ["ru_RU.dic.1", "ru_RU.dic.2"]
    )
    with tempfile.TemporaryDirectory() as temporary:
        dic = Path(temporary) / "ru_RU.dic"
        dic.write_bytes(dictionary)
        accepted = subprocess.check_output(
            [str(args.validator.resolve()), str(resources / "ru_RU.aff"), str(dic)],
            input=raw.encode(),
        ).decode().splitlines()
    words = [[word, int(frequency)] for word, frequency in
             (line.split("\t") for line in accepted)]
    notice_bytes = args.notice.read_bytes()
    if hashlib.sha256(notice_bytes).hexdigest() != NOTICE_SHA256:
        raise ValueError("Unexpected wordfreq attribution notice")
    notice = notice_bytes.decode()
    plain_notice = "\n".join(line.rstrip() for line in notice.splitlines()).rstrip() + "\n"
    metadata = {
        "schema": 1,
        "attribution": "wordfreq, Copyright 2022 Robyn Speer",
        "license": LICENSE,
        "notice": notice,
        "source": "https://github.com/rspeer/wordfreq",
        "revision": REVISION,
        "source_sha256": SOURCE_SHA256,
        "changes": "Russian lowercase words, 1-24 letters, Zipf >= 3.00, validated "
                   "by the bundled Hunspell ru_RU dictionary. Frequencies are Zipf * 100.",
        "dictionary_sha256": hashlib.sha256(dictionary).hexdigest(),
    }
    header = json.dumps(metadata, ensure_ascii=False, indent=2)
    output = header[:-2] + ',\n  "words": [\n' + ",\n".join(
        "    " + json.dumps(row, ensure_ascii=False) for row in words
    ) + "\n  ]\n}\n"
    target = resources / "ru_frequency.json"
    target.write_bytes(output.encode("utf-8"))
    (resources / "LICENSE_ru_frequency.txt").write_bytes((
        "Telegravim Russian suggestion frequency data\n"
        f"Adapted from wordfreq at {REVISION}.\n"
        "This adapted data is distributed under CC BY-SA 4.0:\n"
        f"{LICENSE}\n\n{metadata['changes']}\n\n{plain_notice}"
    ).encode("utf-8"))
    print(json.dumps({"words": len(words), "bytes": len(output.encode()),
                      "sha256": hashlib.sha256(output.encode()).hexdigest()}))


if __name__ == "__main__":
    main()
