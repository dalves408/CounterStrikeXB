"""Build the hi-res texture add-on payload: texture-01.bin ... texture-NN.bin.

    python scripts/build_texture_payload.py [--source analysis/texture-recreated-4x] [--out installer/Output/texture-addon-1.0.0]

The payload is the runtime pack: every DDS file texture_pack_dds/index.tsv names (the files the
renderer can load, src/cs_texture_pack.c) and the index itself. Each staged file is checked
against dds-manifest.json (size and SHA-256). Files are split by raw size into parts that stay
under GitHub's 2 GiB release-asset limit (BC7 hardly compresses, so a part's archive is about its
raw size); each part is an independent 7z (LZMA2) archive with texture_pack/ at its top, and
part 1 also carries manifest.txt, pack-info.json and README.txt. manifest.txt lists every
installed file as "<sha256> <bytes> <path>" (LF, sorted, paths relative to texture_pack); the
add-on Setup pins its SHA-256 and line count.

Every archive is extracted again and compared with the staged tree (path, size, SHA-256) before
it is published. texture-payload.json records what the add-on Setup needs
(scripts/build_texture_addon.py).
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from hashlib import file_digest, sha256
import json
import os
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[1]
SEVEN_ZIP = Path('C:/Program Files/7-Zip/7z.exe')
VERSION = '1.0.0'
PART_RAW_CAP = 1_700_000_000           # raw bytes per part; the archive must stay < 2,000,000,000
ASSET_CAP = 2_000_000_000


def digest(path):
    with open(path, 'rb') as stream:
        return file_digest(stream, 'sha256').hexdigest()


def catalog(root):
    with ThreadPoolExecutor(8) as pool:
        files = [p for p in root.rglob('*') if p.is_file()]
        sums = list(pool.map(digest, files))
    return {p.relative_to(root).as_posix(): (p.stat().st_size, s) for p, s in zip(files, sums)}


def place(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, target)           # same volume: no copy needed; the archive reads the bytes
    except OSError:
        shutil.copyfile(source, target)


def plan(source):
    pack = source / 'texture_pack_dds'
    manifest = {t['file']: t for t in json.loads((source / 'dds-manifest.json').read_text())['textures']}
    lines = (pack / 'index.tsv').read_text(encoding='ascii').splitlines()
    assert lines[0] == 'CSXB-DDS-1', 'unexpected index format'
    files = sorted({line.split()[4] for line in lines[1:]})
    missing = [f for f in files if f not in manifest]
    assert not missing, f'{len(missing)} indexed files are not in dds-manifest.json, e.g. {missing[0]}'
    rows = {f: (manifest[f]['bytes'], manifest[f]['sha256']) for f in files}
    index = pack / 'index.tsv'
    rows['index.tsv'] = (index.stat().st_size, digest(index))
    # Sequential split in path order (keeps a map's files together), balanced by raw size.
    total = sum(size for size, _ in rows.values())
    count = -(-total // PART_RAW_CAP)
    target = total / count
    parts, current, size_now = [], [], 0
    for name in ['index.tsv'] + files:
        size = rows[name][0]
        if current and size_now + size > target and len(parts) < count - 1:
            parts.append(current)
            current, size_now = [], 0
        current.append(name)
        size_now += size
    parts.append(current)
    return pack, rows, parts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--source', type=Path, default=ROOT / 'analysis/texture-recreated-4x')
    ap.add_argument('--out', type=Path, default=ROOT / f'installer/Output/texture-addon-{VERSION}')
    ap.add_argument('--threads', default='4', help='7-Zip threads per archive (-mmt)')
    args = ap.parse_args()
    source, out = args.source.resolve(), args.out.resolve()
    pack_src, rows, parts = plan(source)
    print(f'{len(rows)} files, {sum(s for s, _ in rows.values()):,} bytes, {len(parts)} parts', flush=True)

    manifest = ''.join(f'{sha} {size} {name}\n' for name, (size, sha) in sorted(rows.items()))
    manifest_sha = sha256(manifest.encode('ascii')).hexdigest()
    info = dict(product='Counter-Strike Hi-Res Textures', version=VERSION, format='BC7_UNORM', upscale=4,
                files=len(rows) - 1, installed_texture_bytes=sum(s for n, (s, _) in rows.items() if n != 'index.tsv'),
                categories=dict(sorted(Counter(n.split('/')[0] for n in rows if n != 'index.tsv').items())),
                manifest_sha256=manifest_sha, payload_format='7z', compression='LZMA2')
    readme = ('Counter-Strike (Xbox recompilation) - 4x Hi-Res Textures\n\n'
              'This folder belongs beside cs2_xbox_recomp.exe. The game loads it at start.\n\n'
              'Turn it on or off in the game with F1 > Hi-res textures (it changes at once), or in '
              'config.exe. A higher Render scale (Medium or High) shows the extra detail.\n')
    extra = {'manifest.txt': manifest.encode('ascii'),
             'pack-info.json': (json.dumps(info, indent=2) + '\n').encode('ascii'),
             'README.txt': readme.encode('ascii')}
    tracked = dict(rows)
    for name, data in extra.items():
        if name != 'manifest.txt':
            tracked[name] = (len(data), sha256(data).hexdigest())

    out.mkdir(parents=True, exist_ok=True)
    results = []
    with TemporaryDirectory(dir=out, prefix='.build-') as temp:
        work = Path(temp)
        for number, names in enumerate(parts, 1):
            name = f'texture-{number:02d}.bin'
            stage = work / f'stage{number:02d}'
            pack = stage / 'texture_pack'
            for rel in names:
                place(pack_src / rel, pack / rel)
            if number == 1:
                for rel, data in extra.items():
                    (pack / rel).write_bytes(data)
            staged = catalog(stage)
            expect = {f'texture_pack/{rel}': tracked[rel] for rel in names}
            if number == 1:
                expect.update({f'texture_pack/{rel}': (len(d), sha256(d).hexdigest()) for rel, d in extra.items()})
            bad = [k for k in expect if staged.get(k) != expect[k]]
            assert staged.keys() == expect.keys() and not bad, f'part {number}: staged files differ, e.g. {bad[:3]}'
            archive = work / name
            print(f'{name}: compressing {len(names)} files ({sum(rows[n][0] for n in names):,} bytes)', flush=True)
            subprocess.run([str(SEVEN_ZIP), 'a', '-t7z', '-m0=LZMA2', '-mx=7', '-md=32m', '-ms=256m',
                            f'-mmt={args.threads}', '-mtc=off', '-mta=off', '-bsp0', '-bso0', str(archive),
                            'texture_pack'], cwd=stage, check=True)
            size = archive.stat().st_size
            assert size < ASSET_CAP, f'{name} is {size:,} bytes'
            test = work / f'extract{number:02d}'
            subprocess.run([str(SEVEN_ZIP), 'x', '-t7z', '-bsp0', '-bso0', f'-o{test}', str(archive)], check=True)
            assert catalog(test) == staged, f'{name}: extracted tree differs from the staged one'
            shutil.rmtree(test)
            shutil.rmtree(stage)
            sha = digest(archive)
            archive.replace(out / name)       # published only after the round trip matched
            (out / f'{name}.sha256').write_text(f'{sha}  {name}\n', encoding='ascii')
            results.append(dict(filename=name, bytes=size, sha256=sha,
                                extracted_bytes=sum(v[0] for v in staged.values()), files=len(staged)))
            print(f'{name}: {size:,} bytes, sha256 {sha}', flush=True)

    payload = dict(info, manifest_lines=len(rows), parts=results,
                   extracted_bytes=sum(p['extracted_bytes'] for p in results))
    (out / 'texture-payload.json').write_text(json.dumps(payload, indent=2) + '\n', encoding='ascii')
    (out / 'manifest.txt').write_bytes(manifest.encode('ascii'))
    print(json.dumps(payload, indent=2))


if __name__ == '__main__':
    main()
