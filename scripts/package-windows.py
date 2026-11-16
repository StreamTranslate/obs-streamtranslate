#!/usr/bin/env python3
"""Package the existing Windows binary for current and older OBS loaders."""
import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


def package_windows(dll, certificates, output, updater=None):
    dll = Path(dll)
    certificates = Path(certificates)
    # Read before creating output, so missing inputs cannot leave a partial ZIP.
    binary = dll.read_bytes()
    certificate_data = certificates.read_bytes()
    if not binary.startswith(b'MZ'):
        raise ValueError('Expected a Windows plugin DLL')
    if b'-----BEGIN CERTIFICATE-----' not in certificate_data:
        raise ValueError('Expected a PEM certificate bundle')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    module = 'obs-streamtranslate'
    files = {module + '.dll': binary, 'cacert.pem': certificate_data}
    if updater:
        updater_data = Path(updater).read_bytes()
        if not updater_data.startswith(b'MZ'):
            raise ValueError('Expected a Windows updater executable')
    repo = Path(__file__).resolve().parents[1]
    licenses = [repo / 'LICENSE-libcaption.txt', repo / 'third-party/libwebsockets-LICENSE.txt']
    modern = output / 'StreamTranslate-OBS-Plugin-Windows.zip'
    legacy = output / 'StreamTranslate-OBS-Plugin-Windows-OBS32-Portable.zip'
    with ZipFile(modern, 'w', compression=ZIP_DEFLATED) as archive:
        for license_file in licenses:
            archive.writestr(license_file.name, license_file.read_bytes())
        archive.writestr(f'{module}/data/', b'')
        for name, content in files.items():
            # OBS 33+: new flat layout. OBS 32: same ProgramData module's bin/64bit.
            # OBS 33 loads the modern module first and skips its legacy duplicate.
            archive.writestr(f'{module}/{name}', content)
            archive.writestr(f'{module}/bin/64bit/{name}', content)
        if updater:
            # Both dedicated layouts can start the updater beside their loaded DLL.
            archive.writestr(f'{module}/obs-streamtranslate-updater.exe', updater_data)
            archive.writestr(f'{module}/bin/64bit/obs-streamtranslate-updater.exe', updater_data)
    with ZipFile(legacy, 'w', compression=ZIP_DEFLATED) as archive:
        for license_file in licenses:
            archive.writestr(license_file.name, license_file.read_bytes())
        for name, content in files.items():
            archive.writestr(f'obs-plugins/64bit/{name}', content)
    return modern, legacy


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', required=True)
    parser.add_argument('--certificates', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--updater')
    args = parser.parse_args()
    for path in package_windows(args.dll, args.certificates, args.output, args.updater):
        print(path)
