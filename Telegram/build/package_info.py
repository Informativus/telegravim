import json
from pathlib import Path
import re
import subprocess
import sys


root = Path(__file__).resolve().parents[2]
destination = Path(sys.argv[1])
platform = sys.argv[2]
source = (root / "Telegram/SourceFiles/core/vim_keymap.cpp").read_text()
version = re.search(r'kTelegraVimBuild = "([0-9.\-a-z]+)"', source).group(1)
commit = subprocess.check_output(
    ["git", "rev-parse", "HEAD"], cwd=root, text=True
).strip()
subprocess.run(["git", "diff", "--exit-code", "HEAD"], cwd=root, check=True)
(destination / "BUILD-INFO.json").write_text(
    json.dumps({
        "product": "Telegravim",
        "version": version,
        "commit": commit,
        "platform": platform,
        "configuration": "Release",
        "automatic_updates": False,
    }, indent=2) + "\n",
    encoding="utf-8",
)
(destination / "README.txt").write_text(
    f"Telegravim {version}\n\n"
    "Run Telegram.exe on Windows or ./Telegram on Linux.\n"
    "Updates: https://github.com/Informativus/telegravim/releases\n"
    "Keyboard guide: https://github.com/Informativus/telegravim/blob/main/docs/vim-keymap.md\n\n"
    "View mode: s selects a message by its letter; j/k extends the selection.\n"
    "Shift+R opens reactions; r replies. / searches the Vim help window.\n"
    "This package does not automatically install official Telegram updates.\n",
    encoding="utf-8",
)
