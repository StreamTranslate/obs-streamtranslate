#!/usr/bin/env python3
"""Create desktop update assets from the exact Mac and Windows release packages."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

MAC_ASSET = 'StreamTranslate-OBS-Update-macOS-arm64.zip'
WINDOWS_ASSET = 'StreamTranslate-OBS-Update-Windows-x64.zip'


def package_macos(installer, version, public_key, destination):
    subprocess.run(['xcrun', 'stapler', 'validate', installer], check=True)
    subprocess.run(['spctl', '--assess', '--type', 'install', installer], check=True)
    with tempfile.TemporaryDirectory() as scratch:
        expanded = Path(scratch) / 'package'
        subprocess.run(['pkgutil', '--expand-full', installer, expanded], check=True)
        bundle, = expanded.rglob('obs-streamtranslate.plugin')
        requirement = (
            '=anchor apple generic and identifier "live.streamtranslate.obs-plugin" and '
            'certificate leaf[subject.OU] = "VJSDPHM64R" and '
            'certificate leaf[field.1.2.840.113635.100.6.1.13] exists'
        )
        subprocess.run(['codesign', '--verify', '--deep', '--strict', '-R', requirement, bundle], check=True)
        with (bundle / 'Contents/Info.plist').open('rb') as plist:
            if plistlib.load(plist)['CFBundleVersion'] != version:
                raise ValueError('Mac bundle version does not match the release')
        if public_key not in (bundle / 'Contents/MacOS/obs-streamtranslate-updater').read_bytes():
            raise ValueError('Mac bundle has the wrong update signing key')
        with ZipFile(destination, 'w', ZIP_DEFLATED) as archive:
            for entry in sorted(bundle.rglob('*')):
                if entry.is_symlink() or not (entry.is_file() or entry.is_dir()):
                    raise ValueError('Mac bundle contains a link or unsupported entry')
                archive.write(entry, entry.relative_to(bundle.parent))


def package_windows(package, version, public_key, destination):
    module = 'obs-streamtranslate'
    with ZipFile(package) as source:
        binary = source.read(f'{module}/{module}.dll')
        if (version.encode() + b'\0' not in binary or
                binary != source.read(f'{module}/bin/64bit/{module}.dll')):
            raise ValueError('Windows DLLs do not match the release')
        helper = f'{module}/{module}-updater.exe'
        updater = source.read(helper)
        if public_key not in updater or updater != source.read(f'{module}/bin/64bit/{module}-updater.exe'):
            raise ValueError('Windows updater binaries differ or have the wrong update signing key')
        with ZipFile(destination, 'w', ZIP_DEFLATED) as archive:
            for member in source.infolist():
                if member.filename.startswith(module + '/'):
                    archive.writestr(member, source.read(member))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--mac-pkg', required=True, type=Path)
    parser.add_argument('--windows-zip', required=True, type=Path)
    parser.add_argument('--signing-key', required=True, type=Path, help='Ed25519 private key PEM')
    parser.add_argument('--output', required=True, type=Path, help='New directory for the four assets')
    args = parser.parse_args()
    if not re.fullmatch(r'(0|[1-9][0-9]{0,5})(\.(0|[1-9][0-9]{0,5})){2}', args.version):
        parser.error('expected a numeric release version such as 0.1.17')

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists():
        parser.error('output directory already exists')
    public_der = subprocess.check_output([
        'openssl', 'pkey', '-in', args.signing_key, '-pubout', '-outform', 'DER',
    ])
    ed25519_prefix = bytes.fromhex('302a300506032b6570032100')
    if not public_der.startswith(ed25519_prefix) or len(public_der) != len(ed25519_prefix) + 32:
        parser.error('expected an Ed25519 signing key')
    public_key = base64.b64encode(public_der[len(ed25519_prefix):])

    with tempfile.TemporaryDirectory(dir=args.output.parent) as scratch:
        stage = Path(scratch) / 'assets'
        stage.mkdir()
        package_macos(args.mac_pkg, args.version, public_key, stage / MAC_ASSET)
        package_windows(args.windows_zip, args.version, public_key, stage / WINDOWS_ASSET)
        assets = {}
        for name in [MAC_ASSET, WINDOWS_ASSET]:
            with (stage / name).open('rb') as archive:
                assets[name] = {
                    'sha256': hashlib.file_digest(archive, 'sha256').hexdigest(),
                    'size': (stage / name).stat().st_size,
                }
        manifest = stage / 'obs-update.json'
        manifest.write_text(json.dumps({'version': args.version, 'assets': assets}) + '\n')
        signature = subprocess.check_output([
            'openssl', 'pkeyutl', '-sign', '-rawin', '-inkey', args.signing_key, '-in', manifest,
        ])
        if len(signature) != 64:
            raise ValueError('expected an Ed25519 signature')
        (stage / 'obs-update.sig').write_bytes(base64.b64encode(signature) + b'\n')
        stage.rename(args.output)


if __name__ == '__main__':
    main()
