"""Compile the hi-res texture add-on Setup against a verified payload.

    python scripts/build_texture_addon.py installer/Output/texture-addon-1.0.0/texture-payload.json [--url-base URL] [--output DIR]

Without --url-base Setup only uses the texture-NN.bin parts beside it. With --url-base (the release
folder the parts are uploaded to, e.g. https://github.com/dalves408/CounterStrikeXB/releases/download/textures-1.0.0/)
it downloads missing parts from <url-base>texture-NN.bin, with resume. Every part's size and
SHA-256 are pinned from texture-payload.json either way.

Needs Inno Setup 6.4+ (ArchiveExtraction) and the cs_texture_download target built in
build_release. Output: CounterStrike-HiRes-Textures-<version>-Setup.exe and its .sha256.
"""
import argparse
from hashlib import file_digest
import json
import os
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
# Inno Setup 6: ISCC_EXE if set, else this user's own install (the default for a non-admin install), else a
# machine-wide one -- not one PC's user folder.
COMPILERS = [Path(p) for p in (os.environ.get('ISCC_EXE', ''),
                               os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Programs', 'Inno Setup 6', 'ISCC.exe'),
                               'C:/Program Files (x86)/Inno Setup 6/ISCC.exe',
                               'C:/Program Files/Inno Setup 6/ISCC.exe') if p]
HELPER = ROOT / 'build_release/RelWithDebInfo/cs_texture_download.exe'


def digest(path):
    with open(path, 'rb') as stream:
        return file_digest(stream, 'sha256').hexdigest()


# The release the published pack's parts are attached to (scripts/build_texture_payload.py output,
# pinned in installer/texture-payload-<version>.json). Players can only download them while the
# repository is public.
RELEASE_URL_BASE = 'https://github.com/dalves408/CounterStrikeXB/releases/download/textures-{version}/'


def write_includes(payload, folder, url_base=None):
    """texture_payload.iss (defines) and texture_parts_code.iss (each part's URL, size and SHA-256)
    for installer/texture_download_code.iss, used by the add-on and by the game Setup (cs.iss)."""
    parts = payload['parts']
    total = sum(p['bytes'] for p in parts)
    defines = dict(TextureVersion=payload['version'], TexturePartCount=len(parts),
                   TextureExtractedBytes=payload['extracted_bytes'],
                   TextureManifestSHA256=payload['manifest_sha256'], TextureFileCount=payload['manifest_lines'],
                   TextureCacheRoot=r'{localappdata}\CounterStrikeXB\Downloads',
                   TextureDownloadBytes=total,
                   TextureDownloadText=f'{len(parts)} parts, {total / 1e9:.1f} GB in all',
                   TextureInstalledText=f'{payload["extracted_bytes"] / 1e9:.1f} GB')
    lines = [f'#define {k} "{v}"' if isinstance(v, str) else f'#define {k} {v}' for k, v in defines.items()]
    if url_base:
        lines.append('#define TextureHasDownload')
    (folder / 'texture_payload.iss').write_text('\n'.join(lines) + '\n', encoding='ascii')
    code = ['procedure TexturePartInfo(Part: Integer; var Url, SHA: String; var Bytes: Int64);',
            'begin', '  case Part of']
    for number, part in enumerate(parts, 1):
        assert part['filename'] == f'texture-{number:02d}.bin'
        url = (url_base + part['filename']) if url_base else 'https://unused.invalid/' + part['filename']
        code += [f"    {number}: begin Url := '{url}'; SHA := '{part['sha256']}'; Bytes := {part['bytes']}; end;"]
    code += ["  else RaiseException('Unknown texture part');", '  end;', 'end;']
    (folder / 'texture_parts_code.iss').write_text('\n'.join(code) + '\n', encoding='ascii')


def compiler():
    found = next((c for c in COMPILERS if c.exists()), None) or shutil.which('ISCC')
    if not found:
        raise SystemExit('Inno Setup 6 (ISCC.exe) not found')
    return Path(found)


def build(metadata, output, url_base=None):
    payload = json.loads(metadata.read_text(encoding='ascii'))
    parts = payload['parts']
    if not HELPER.exists():
        raise SystemExit(f'missing {HELPER}: cmake --build build_release --config RelWithDebInfo --target cs_texture_download')
    output.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=ROOT / 'installer', prefix='.addon-') as temp:
        stage = Path(temp)
        installer, resources = stage / 'installer', stage / 'resources'
        installer.mkdir()
        resources.mkdir()
        for name in ('cs_textures.iss', 'texture_download_code.iss'):
            shutil.copyfile(ROOT / 'installer' / name, installer / name)
        images = sorted((ROOT / 'installer').glob('wizard_*.bmp')) + sorted((ROOT / 'installer').glob('wizard_back_*.png'))
        if len(images) != 13:
            raise SystemExit('installer/wizard_*.bmp, wizard_back_*.png missing: run scripts/build_installer.py once to make them')
        for image in images:
            shutil.copyfile(image, installer / image.name)
        shutil.copyfile(ROOT / 'resources/cs.ico', resources / 'cs.ico')
        shutil.copyfile(HELPER, installer / 'cs_texture_download.exe')

        write_includes(payload, installer, url_base)

        subprocess.run([str(compiler()), '/Qp', f'/O{output}', str(installer / 'cs_textures.iss')], check=True)

    setup = output / f'CounterStrike-HiRes-Textures-{payload["version"]}-Setup.exe'
    (output / (setup.name + '.sha256')).write_text(f'{digest(setup)}  {setup.name}\n', encoding='ascii')
    return setup


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('metadata', type=Path)
    ap.add_argument('--output', type=Path)
    ap.add_argument('--url-base', help='Folder URL the texture-NN.bin parts are hosted in (HTTPS; HTTP loopback for tests).')
    args = ap.parse_args()
    url_base = args.url_base
    if url_base:
        if not url_base.endswith('/'):
            url_base += '/'
        u = urlsplit(url_base)
        loopback = u.scheme == 'http' and u.hostname in ('localhost', '127.0.0.1')
        if not u.hostname or (u.scheme != 'https' and not loopback) or any(c.isspace() or c in '"\'{}' for c in url_base):
            ap.error('use an HTTPS URL (or HTTP loopback for tests)')
    metadata = args.metadata.resolve()
    print(build(metadata, (args.output or metadata.parent).resolve(), url_base))


if __name__ == '__main__':
    main()
