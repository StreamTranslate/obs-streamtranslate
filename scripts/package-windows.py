#!/usr/bin/env python3
"""Package the existing Windows binary for current and older OBS loaders."""
import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


def package_windows(dll, certificates, output):
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
    modern = output / 'StreamTranslate-OBS-Plugin-Windows.zip'
    legacy = output / 'StreamTranslate-OBS-Plugin-Windows-OBS32-Portable.zip'
    with ZipFile(modern, 'w', compression=ZIP_DEFLATED) as archive:
        archive.writestr(f'{module}/data/', b'')
        for name, content in files.items():
            # OBS 33+: new flat layout. OBS 32: same ProgramData module's bin/64bit.
            # OBS 33 loads the modern module first and skips its legacy duplicate.
            archive.writestr(f'{module}/{name}', content)
            archive.writestr(f'{module}/bin/64bit/{name}', content)
    with ZipFile(legacy, 'w', compression=ZIP_DEFLATED) as archive:
        for name, content in files.items():
            archive.writestr(f'obs-plugins/64bit/{name}', content)
    return modern, legacy


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', required=True)
    parser.add_argument('--certificates', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    for path in package_windows(args.dll, args.certificates, args.output):
        print(path)
