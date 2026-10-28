#!/usr/bin/env python3
"""Prepare native update envelopes and publish a complete, signed release catalog."""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

from sign_update import der_to_raw_rs

ROOT = Path(__file__).resolve().parents[2]
KEYS = ROOT / 'Telegram/Resources/update'
REPOSITORY = 'Informativus/telegravim'
PLATFORMS = {
    'macos-arm64': ('armac', 'mac', 'arm', 'Telegravim-macos-arm64', 'macos.yml'),
    'windows-x64': ('win64', 'win', 'x64', 'Telegravim-windows-x64', 'win.yml'),
    'linux-x86_64': ('linux', 'linux', 'x64', 'Telegravim-linux-x86_64', 'linux.yml'),
}


def run(*args, **kwargs):
    return subprocess.check_output([str(arg) for arg in args], **kwargs)


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def version():
    return int(re.search(r'^AppVersion\s+(\d+)$', (ROOT / 'Telegram/build/version').read_text(), re.M)[1])


def pack(args):
    stage = args.stage.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = run('git', 'rev-parse', 'HEAD', cwd=ROOT, text=True).strip()
    run('git', 'diff', '--exit-code', 'HEAD', cwd=ROOT)
    platform = PLATFORMS[args.platform]
    required = ('Telegram.app/Contents/Frameworks/Updater' if args.platform == 'macos-arm64'
                else 'Updater.exe' if args.platform == 'windows-x64' else 'Updater')
    if not (stage / required).is_file():
        raise ValueError('The staged package lacks its native updater')
    for item in stage.rglob('*'):
        if item == stage / 'Applications':
            continue
        if item.is_symlink():
            raise ValueError('Native update format cannot preserve staged symlinks')
    command = [args.packer.resolve()]
    for item in sorted(stage.iterdir()):
        if item.name == 'Applications':
            continue
        command.extend(['-path', item])
    command.extend(['-version', version(), '-channel', 'stable', '-keys-loc', KEYS,
                    '-emit-signing-input', output / 'signing-input'])
    if args.platform == 'macos-arm64':
        command.extend(['-arch', 'arm64'])
    elif args.platform == 'windows-x64':
        command.extend(['-target', 'win64'])
    run(*command, cwd=output)
    name = f'td-update-{platform[1]}-{platform[2]}-{version()}.unsigned'
    envelope = output / name
    if not envelope.is_file() or envelope.stat().st_size > 256 * 1024 * 1024:
        raise ValueError('Missing or oversized native update envelope')
    metadata = {'source_sha': source, 'platform': args.platform, 'native_version': version(),
                'artifact': name, 'sha256': sha256(envelope)}
    (output / f'update-{args.platform}.json').write_text(json.dumps(metadata, indent=2) + '\n')
    (output / 'signing-input').unlink()


def api(path):
    return json.loads(run('gh', 'api', f'repos/{REPOSITORY}/{path}', text=True))


def download_run(run_id, artifact_name, workflow, source, destination):
    if not re.fullmatch(r'[1-9][0-9]*', run_id):
        raise ValueError('Invalid build run id')
    metadata = api(f'actions/runs/{run_id}')
    if (metadata['conclusion'] != 'success' or metadata['event'] != 'workflow_dispatch'
            or metadata['path'] != '.github/workflows/' + workflow
            or metadata['repository']['full_name'] != REPOSITORY or metadata['head_sha'] != source):
        raise ValueError('Build run did not successfully execute the expected release workflow')
    destination.mkdir(parents=True)
    run('gh', 'run', 'download', run_id, '--repo', REPOSITORY,
        '--name', artifact_name, '--dir', destination)
    for path in destination.rglob('*'):
        if path.is_symlink():
            raise ValueError('Release artifact contains a symlink')
    return destination


def sign(data, key, directory):
    message = directory / 'signing-input'
    signature = directory / 'signature.der'
    message.write_bytes(data)
    run('openssl', 'dgst', '-sha256', '-sign', key, '-out', signature, message)
    run('openssl', 'dgst', '-sha256', '-verify', KEYS / 'catalog-public.pem',
        '-signature', signature, message)
    return der_to_raw_rs(signature.read_bytes())


def sign_envelope(path, platform, native_version, key, directory, packer, existing=None):
    data = path.read_bytes()
    if len(data) > 256 * 1024 * 1024 or len(data) < 31:
        raise ValueError('Invalid envelope size')
    os_id = {'win': 0, 'mac': 1, 'linux': 2}[platform[1]]
    arch_id = {'x86': 0, 'x64': 1, 'arm': 2}[platform[2]]
    if (data[:8] != b'TDUP\x02\x00\x00\x00' or data[8:11] != bytes([0, os_id, arch_id])
            or struct.unpack_from('<Q', data, 11)[0] != native_version << 32):
        raise ValueError('Wrong native update channel, target or version')
    offset = 27

    def blob(limit):
        nonlocal offset
        if offset + 4 > len(data):
            raise ValueError('Truncated envelope')
        length = struct.unpack_from('<I', data, offset)[0]
        offset += 4
        if length > limit or offset + length > len(data):
            raise ValueError('Invalid envelope field size')
        value = data[offset:offset + length]
        offset += length
        return value

    manifest = blob(1024 * 1024)
    signed_region = data[:offset]
    root_signature = blob(64)
    if manifest != (KEYS / 'manifest.min.json').read_bytes() or root_signature != (KEYS / 'manifest.sig').read_bytes():
        raise ValueError('Envelope was built against different release trust')
    signature_offset = offset
    if offset + 4 > len(data) or struct.unpack_from('<I', data, offset)[0] != 0:
        raise ValueError('Expected an unsigned envelope')
    offset += 4
    payload_offset = offset
    payload = blob(256 * 1024 * 1024)
    if offset != len(data) or not payload:
        raise ValueError('Invalid envelope payload')
    key_id = b'televim-release-1'
    header = data[:signature_offset] + struct.pack('<II', 1, len(key_id)) + key_id + struct.pack('<I', 64)
    if existing is not None:
        previous = existing.read_bytes()
        if len(previous) != len(header) + 64 + len(data[payload_offset:]) or not previous.startswith(header) or previous[len(header) + 64:] != data[payload_offset:]:
            raise ValueError('Previously published envelope differs from the exact build')
        signature = previous[len(header):len(header) + 64]
    else:
        signature = sign(signed_region + hashlib.sha256(payload).digest(), key, directory)
    signature_file = directory / 'envelope-signature'
    signature_file.write_bytes(signature)
    run(packer, '-unsigned', path, '-keys-loc', KEYS,
        '-embed-signatures', 'televim-release-1:' + str(signature_file))
    result = path.with_suffix('').read_bytes()
    if result != header + signature + data[payload_offset:]:
        raise ValueError('Native envelope verification produced unexpected bytes')
    return result


def previous_catalog(directory, bootstrap):
    try:
        with urllib.request.urlopen('https://informativus.github.io/telegravim/updates/current6', timeout=30) as response:
            encoded = response.read(1024 * 1024 + 1)
    except urllib.error.HTTPError as error:
        if error.code == 404 and bootstrap:
            return None
        raise
    if len(encoded) > 1024 * 1024:
        raise ValueError('Current catalog is too large')
    wrapper = json.loads(encoded)
    payload = base64.b64decode(wrapper['payload'], validate=True)
    raw = base64.b64decode(wrapper['signature'], validate=True)
    if len(raw) != 64:
        raise ValueError('Current catalog signature is invalid')
    integers = []
    for component in (raw[:32], raw[32:]):
        value = component.lstrip(b'\0') or b'\0'
        if value[0] & 128:
            value = b'\0' + value
        integers.append(b'\x02' + bytes([len(value)]) + value)
    body = b''.join(integers)
    signature = directory / 'previous.sig'
    message = directory / 'previous.message'
    signature.write_bytes(b'\x30' + bytes([len(body)]) + body)
    message.write_bytes(b'televim-catalog-v1\n' + payload)
    run('openssl', 'dgst', '-sha256', '-verify', KEYS / 'catalog-public.pem',
        '-signature', signature, message)
    return json.loads(payload)


def release(args):
    if not re.fullmatch(r'[0-9]{4}\.[0-9]{2}\.[0-9]{2}-[0-9]+', args.tag):
        raise ValueError('Only stable release tags are supported')
    source = run('git', 'rev-parse', args.tag + '^{commit}', cwd=ROOT, text=True).strip()
    current = run('git', 'rev-parse', 'HEAD', cwd=ROOT, text=True).strip()
    if source != current:
        raise ValueError('Publication code must come from the exact release commit')
    run('git', 'merge-base', '--is-ancestor', source, 'origin/main', cwd=ROOT)
    release_info = api('releases/tags/' + args.tag)
    if release_info['prerelease']:
        raise ValueError('Stable catalog cannot publish a prerelease')
    existing_assets = {asset['name']: asset for asset in release_info['assets']}
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    assets = output / 'assets'
    assets.mkdir()
    native_version = version()
    android_version = int(re.search(r'^APP_VERSION_CODE=(\d+)$',
        (ROOT / 'android/overlay/gradle.properties').read_text(), re.M)[1]) * 10 + 9
    run('openssl', 'pkeyutl', '-verify', '-pubin', '-rawin', '-inkey', KEYS / 'root-public.pem',
        '-in', KEYS / 'manifest.min.json', '-sigfile', KEYS / 'manifest.sig')
    with tempfile.TemporaryDirectory(dir=output) as temporary:
        work = Path(temporary)
        previous = previous_catalog(work, args.bootstrap)
        if previous is not None:
            same_release = previous['release'] == args.tag and previous['source_sha'] == source
            versions = [previous['desktop'][target[0]]['stable']['released'] for target in PLATFORMS.values()]
            android_previous = previous['android']['stable']['version_code']
            if (not same_release and (native_version <= max(versions) or android_version <= android_previous)
                    or same_release and (any(value != native_version for value in versions) or android_version != android_previous)):
                raise ValueError('Release would roll back or reuse a native version')
        desktop = {}
        for name, run_id in [('macos-arm64', args.mac_run), ('windows-x64', args.win_run), ('linux-x86_64', args.linux_run)]:
            platform = PLATFORMS[name]
            folder = download_run(run_id, platform[3], platform[4], source, work / name)
            metadata = json.loads((folder / f'update-{name}.json').read_text())
            expected = f'td-update-{platform[1]}-{platform[2]}-{native_version}.unsigned'
            if (metadata['source_sha'] != source or metadata['native_version'] != native_version
                    or metadata['platform'] != name or metadata['artifact'] != expected
                    or sha256(folder / expected) != metadata['sha256']):
                raise ValueError('Desktop package source, version or digest mismatch')
            signed = assets / expected.removesuffix('.unsigned')
            previous = None
            if signed.name in existing_assets:
                run('gh', 'release', 'download', args.tag, '--repo', REPOSITORY, '--pattern', signed.name, '--dir', work)
                previous = work / signed.name
            signed.write_bytes(sign_envelope(folder / expected, platform, native_version, args.key, work, args.packer, previous))
            installers = list(folder.glob('*.dmg')) + list(folder.glob('*.zip')) + list(folder.glob('*.tar.xz'))
            if len(installers) != 1:
                raise ValueError('Each Desktop build must include exactly one bootstrap installer')
            shutil.copyfile(installers[0], assets / installers[0].name)
            desktop[platform[0]] = {'stable': {'released': native_version, 'sha256': sha256(signed), 'size': signed.stat().st_size,
                'link': f'https://github.com/{REPOSITORY}/releases/download/{args.tag}/{signed.name}'}}
        folder = download_run(args.android_run, 'Televim-android-universal', 'android.yml', source, work / 'android')
        metadata = json.loads((folder / 'source.json').read_text())
        apk = folder / 'Televim-android-universal.apk'
        if (metadata['source_sha'] != source or metadata['application_id'] != 'org.informativus.televim'
                or int(metadata['version_code']) != android_version or metadata['artifact'] != apk.name
                or set(metadata['abis']) != {'armeabi-v7a', 'arm64-v8a', 'x86', 'x86_64'}
                or metadata['sha256'] != sha256(apk)):
            raise ValueError('Android package source, version, architectures or digest mismatch')
        shutil.copyfile(apk, assets / apk.name)
        apk_info = {'package': metadata['application_id'], 'version': metadata['version_name'],
            'version_code': android_version, 'min_sdk': 26, 'size': apk.stat().st_size,
            'sha256': metadata['sha256'], 'changelog': args.notes.read_text()[:16384],
            'url': f'https://github.com/{REPOSITORY}/releases/download/{args.tag}/{apk.name}'}
        now = int(time.time())
        catalog = {'format': 1, 'issued': now, 'expires': now + 365 * 86400,
                   'release': args.tag, 'source_sha': source, 'desktop': desktop, 'android': {'stable': apk_info}}
        payload = json.dumps(catalog, separators=(',', ':'), ensure_ascii=True).encode()
        signature = sign(b'televim-catalog-v1\n' + payload, args.key, work)
        signed_catalog = json.dumps({'payload': base64.b64encode(payload).decode(),
                                    'signature': base64.b64encode(signature).decode()}, separators=(',', ':')).encode()
    existing = {asset['name']: asset for asset in release_info['assets']}
    for path in assets.iterdir():
        remote = existing.get(path.name)
        if remote:
            if remote.get('digest') != 'sha256:' + sha256(path):
                raise ValueError('Refusing to replace an existing release asset')
        elif not release_info['draft']:
            raise ValueError('Published release is incomplete; create a new release')
    if release_info['draft']:
        for path in sorted(assets.iterdir()):
            if path.name not in existing:
                run('gh', 'release', 'upload', args.tag, path, '--repo', REPOSITORY)
    verified = {asset['name']: asset for asset in api('releases/tags/' + args.tag)['assets']}
    for path in assets.iterdir():
        asset = verified.get(path.name)
        if not asset or asset.get('digest') != 'sha256:' + sha256(path) or asset['size'] != path.stat().st_size:
            raise ValueError('Uploaded release asset failed integrity verification')
    if release_info['draft']:
        run('gh', 'release', 'edit', args.tag, '--repo', REPOSITORY, '--draft=false', '--latest')
    site = output / 'site/updates'
    site.mkdir(parents=True)
    (site / 'current6').write_bytes(signed_catalog)
    (site / 'release.json').write_text(json.dumps({'release': args.tag, 'source_sha': source,
        'desktop_version': native_version, 'android_version': android_version,
        'expires': catalog['expires']}, indent=2) + '\n')
    print('Verified all four platform packages; release published and signed catalog ready for atomic deployment.')


def refresh(args):
    source = run('git', 'rev-parse', 'HEAD', cwd=ROOT, text=True).strip()
    run('git', 'merge-base', '--is-ancestor', source, 'origin/main', cwd=ROOT)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output) as temporary:
        work = Path(temporary)
        catalog = previous_catalog(work, False)
        info = api('releases/tags/' + catalog['release'])
        if info['draft'] or info['prerelease']:
            raise ValueError('The current catalog does not name a published stable release')
        assets = {asset['name']: asset for asset in info['assets']}
        entries = [catalog['desktop'][platform[0]]['stable'] for platform in PLATFORMS.values()]
        entries.append(catalog['android']['stable'])
        for entry in entries:
            url = entry.get('url', entry.get('link'))
            prefix = f'https://github.com/{REPOSITORY}/releases/download/{catalog["release"]}/'
            if not url.startswith(prefix) or '/' in url[len(prefix):]:
                raise ValueError('Current catalog contains an unexpected package URL')
            asset = assets.get(url[len(prefix):])
            if not asset or asset.get('digest') != 'sha256:' + entry['sha256'] or asset['size'] != entry['size']:
                raise ValueError('Current release package is missing or has changed')
        catalog['issued'] = int(time.time())
        catalog['expires'] = catalog['issued'] + 365 * 86400
        payload = json.dumps(catalog, separators=(',', ':'), ensure_ascii=True).encode()
        signature = sign(b'televim-catalog-v1\n' + payload, args.key, work)
        signed = json.dumps({'payload': base64.b64encode(payload).decode(),
                            'signature': base64.b64encode(signature).decode()}, separators=(',', ':')).encode()
    site = output / 'site/updates'
    site.mkdir(parents=True)
    (site / 'current6').write_bytes(signed)
    print('Existing immutable release verified; catalog expiry refreshed without changing any package.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    prepare = commands.add_parser('pack')
    prepare.add_argument('--packer', type=Path, required=True)
    prepare.add_argument('--stage', type=Path, required=True)
    prepare.add_argument('--output', type=Path, required=True)
    prepare.add_argument('--platform', choices=PLATFORMS, required=True)
    publish = commands.add_parser('release')
    publish.add_argument('--tag', required=True)
    publish.add_argument('--packer', type=Path, required=True)
    publish.add_argument('--bootstrap', action='store_true')
    for platform in ['mac', 'win', 'linux', 'android']:
        publish.add_argument('--' + platform + '-run', required=True)
    publish.add_argument('--key', type=Path, required=True)
    publish.add_argument('--notes', type=Path, required=True)
    publish.add_argument('--output', type=Path, required=True)
    renew = commands.add_parser('refresh')
    renew.add_argument('--key', type=Path, required=True)
    renew.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        {'pack': pack, 'release': release, 'refresh': refresh}[args.command](args)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, 'Update release refused: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
