"""Build the portable Windows zip that sits next to the installer on a release.

Layout of the archive, unchanged since 0.1.0:

    Rivage Reverb.vst3/Contents/x86_64-win/Rivage Reverb.vst3
    Rivage Reverb.vst3/Contents/Resources/moduleinfo.json
    Rivage Reverb.vst3/Contents/Resources/LICENSE.txt
    Rivage Reverb.vst3/Contents/Resources/THIRD-PARTY.txt
    INSTALL.txt

Only the plug-in bundle is machine-generated; the three text files are the
repository's own licence and notice documents, so the archive can never ship a
stale copy of them.

Usage:  python scripts/make_zip.py [--build-dir build] [--out dist]
"""

import argparse
import hashlib
import pathlib
import re
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUNDLE_NAME = 'Rivage Reverb.vst3'

INSTALL_TEXT = """Rivage Reverb {version} - Windows x64 VST3

Portable package. Nothing is written outside the folder you copy.

  1. Unpack the archive.
  2. Copy the whole "{bundle}" folder into the VST3 folder your host scans,
     usually

         C:\\Program Files\\Common Files\\VST3

     or the per-user

         %LOCALAPPDATA%\\Programs\\Common\\VST3

  3. Start (or rescan in) your host and look for "Rivage Reverb" under WTdusk.
  4. To uninstall, delete the "{bundle}" folder.

The bundle contains a 64-bit Windows VST3 only: no VST2, AU or AAX builds, and
no other platform. The plug-in is unsigned, so Windows and some hosts warn the
first time it is scanned; the release page lists the SHA-256 of this archive.

Licence: AGPL-3.0-or-later, see LICENSE.txt inside Contents\\Resources.
"""


def project_version() -> str:
    text = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
    match = re.search(r'VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)', text)
    if not match:
        raise SystemExit('could not read the version from CMakeLists.txt')
    return match.group(1)


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', default='build')
    parser.add_argument('--out', default='dist')
    args = parser.parse_args()

    version = project_version()
    bundle = ROOT / args.build_dir / 'Source' / 'RivageReverb_artefacts' / 'Release' / 'VST3' / BUNDLE_NAME
    if not (bundle / 'Contents' / 'x86_64-win' / BUNDLE_NAME).is_file():
        raise SystemExit(f'no release build at {bundle}; run scripts\\build.cmd first')

    out_dir = ROOT / args.out
    out_dir.mkdir(parents=True, exist_ok=True)
    archive = out_dir / f'RivageReverb-{version}-win64-vst3.zip'

    licence = (ROOT / 'LICENSE').read_text(encoding='utf-8')
    notices = (ROOT / 'THIRD-PARTY.md').read_text(encoding='utf-8')
    install = INSTALL_TEXT.format(version=version, bundle=BUNDLE_NAME)

    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in sorted(bundle.rglob('*')):
            if path.is_file():
                zf.write(path, path.relative_to(bundle.parent).as_posix())
        zf.writestr(f'{BUNDLE_NAME}/Contents/Resources/LICENSE.txt', licence)
        zf.writestr(f'{BUNDLE_NAME}/Contents/Resources/THIRD-PARTY.txt', notices)
        zf.writestr('INSTALL.txt', install)

    print(f'zip     : {archive}')
    print(f'size    : {archive.stat().st_size} bytes')
    print(f'sha256  : {sha256(archive)}')
    with zipfile.ZipFile(archive) as zf:
        for info in zf.infolist():
            print(f'  {info.file_size:>8}  {info.filename}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
