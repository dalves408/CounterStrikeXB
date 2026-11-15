"""Install the hi-res texture add-on into a throwaway game folder and check every file.

    python scripts/test_texture_addon.py <Setup.exe> [--work DIR] [--download]

Runs Setup silently against a stand-in game folder and checks the installed texture_pack against
the pinned manifest (every file's size and SHA-256) and the completion marker. With --download,
Setup is copied alone into a scratch folder, so it must download every part from its release URLs
(cache in a scratch folder via /TEXTURECACHE=, deleted after with /KEEPTEXTUREBIN=0).
"""
import argparse
from hashlib import file_digest
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with open(path, 'rb') as stream:
        return file_digest(stream, 'sha256').hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('setup', type=Path)
    ap.add_argument('--work', type=Path, default=ROOT / 'runs/texture_addon_test')
    ap.add_argument('--download', action='store_true')
    args = ap.parse_args()
    setup, work = args.setup.resolve(), args.work.resolve()
    shutil.rmtree(work, ignore_errors=True)
    game, cache = work / 'game', work / 'cache'
    game.mkdir(parents=True)
    cache.mkdir()
    (game / 'cs2_xbox_recomp.exe').write_bytes(b'MZ test stand-in')
    if args.download:                  # alone in a folder: no parts beside it
        shutil.copy2(setup, work / setup.name)
        setup = work / setup.name
    log = work / 'setup.log'
    code = subprocess.run([str(setup), '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', f'/DIR={game}',
                           f'/LOG={log}', f'/TEXTURECACHE={cache}', '/KEEPTEXTUREBIN=0']).returncode
    text = log.read_text(encoding='utf-8', errors='replace')
    assert code == 0, f'Setup exit {code}; see {log}'
    if args.download:
        assert 'READY local' not in text, 'a part was taken from beside Setup'
        assert text.count('READY cache') >= 1, 'nothing was downloaded'
    pack = game / 'texture_pack'
    lines = (pack / 'manifest.txt').read_text(encoding='ascii').splitlines()
    bad = []
    for line in lines:
        sha, size, rel = line.split(' ', 2)
        p = pack / rel
        if not p.is_file() or p.stat().st_size != int(size) or digest(p) != sha:
            bad.append(rel)
    assert not bad, f'{len(bad)} installed files differ, e.g. {bad[:3]}'
    assert (pack / 'cs-addon-complete.txt').read_text().strip() == digest(pack / 'manifest.txt'), 'marker'
    assert not list(game.glob('.texture-stage-*')), 'staging folder left behind'
    assert not list(cache.rglob('texture.bin')), 'download cache not deleted'
    print(f'installed and verified {len(lines)} files' + (' (downloaded)' if args.download else ''))
    shutil.rmtree(work)


if __name__ == '__main__':
    main()
