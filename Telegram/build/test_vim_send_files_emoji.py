#!/usr/bin/env python3
"""Build and run the account-free macOS attachment emoji keyboard regression."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    if sys.platform != 'darwin':
        raise SystemExit('This launcher requires the configured macOS build tree.')
    root = Path(__file__).resolve().parents[2]
    slot = root / 'Telegram/SourceFiles/test/test_scenario.cpp'
    original = slot.read_bytes()
    tracked = subprocess.check_output(
        ['git', 'show', 'HEAD:Telegram/SourceFiles/test/test_scenario.cpp'], cwd=root)
    if original != tracked:
        raise SystemExit('The scenario slot already has local changes; leaving it untouched.')
    builds = root / '.local/builds'
    builds.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='vim-send-files-emoji-', dir=builds))
    profile = work / 'profile'
    profile.mkdir(mode=0o700)
    (profile / 'testing').touch()
    evidence = work / 'evidence'
    evidence.mkdir()
    overlay = b'#include "tests/vim_send_files_emoji_scenario.cpp"\n'
    env = dict(os.environ, TDESKTOP_TEST_EVIDENCE_DIR=str(evidence))
    env.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    try:
        slot.write_bytes(overlay)
        with (work / 'build.log').open('wb') as log:
            subprocess.run(
                ['cmake', '--build', 'out', '--config', 'Debug', '--target',
                 'Telegram', '--', '-quiet'],
                cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        with (work / 'app.log').open('wb') as log:
            subprocess.run(
                ['/usr/bin/sandbox-exec', '-p', '(version 1)(allow default)(deny network-outbound (remote ip "*:*"))',
                 str(root / 'out/Debug/Telegram.app/Contents/MacOS/Telegram'),
                 '-testagent', '-noupdate', '-many', '-workdir', str(profile)],
                cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                timeout=90, check=True)
        result = (evidence / 'test_log.txt').read_text()
        required = {
            'TEST_COMPLETE',
            'SCENARIO_RESULT: PASS (failures: 0)',
            'TEST_RESULT: PASS: mouse opening gives keyboard focus to grid',
            'TEST_RESULT: PASS: h j k l move between actual emoji cells',
            'TEST_RESULT: PASS: j k are inserted into picker search',
            'TEST_RESULT: PASS: empty results and modified Enter never send attachments',
            'TEST_RESULT: PASS: parent picker cannot steal nested dialog keys',
            'TEST_RESULT: PASS: all picker operations leave photos unsent',
        }
        lines = set(result.splitlines())
        if not required <= lines or 'TEST_RESULT: FAIL:' in result:
            raise SystemExit('Attachment emoji regression failed; see the evidence directory.')
        print(f"Passed {result.count('TEST_RESULT: PASS:')} attachment emoji keyboard checks.")
    finally:
        if slot.read_bytes() == overlay:
            slot.write_bytes(original)
        else:
            print('Scenario slot changed concurrently; preserved the new content.', file=sys.stderr)
        print(f'Evidence: {work}')


if __name__ == '__main__':
    main()
