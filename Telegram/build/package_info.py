import json
from pathlib import Path
import re
import subprocess
import sys


root = Path(__file__).resolve().parents[2]
destination = Path(sys.argv[1])
platform = sys.argv[2]
launch = {
    "macos-arm64": "Open the disk image and drag Telegram.app to Applications.",
    "windows-x64": "Extract the entire folder and run Telegram.exe.",
    "linux-x86_64": "Extract the archive and run Telegravim/Telegram.",
}[platform]
source = (root / "Telegram/SourceFiles/core/vim_keymap.cpp").read_text(encoding="utf-8")
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
    f"{launch}\n"
    "Updates: https://github.com/Informativus/telegravim/releases\n"
    "Keyboard guide: https://github.com/Informativus/telegravim/blob/main/docs/vim-keymap.md\n\n"
    "View mode: s selects a message by its letter; j/k extends the selection.\n"
    "Shift+R opens reactions; r replies. / searches the Vim help window.\n"
    "Cmd+Shift+V (macOS) or Ctrl+Shift+V (Windows/Linux) leaves search\n"
    "at the selected position and starts message text selection.\n"
    "This package does not automatically install official Telegram updates.\n",
    encoding="utf-8",
)
