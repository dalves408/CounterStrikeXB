"""Create the CounterStrikeXB D3D11 lookup index from an extracted texture pack."""

import argparse
import json
from pathlib import Path
import re
import zlib


PACKED_FORMATS = {0x06, 0x07, 0x0C, 0x0E, 0x0F, 0x3A, 0x3C}
PATH_PATTERN = re.compile(r'[A-Za-z0-9._{}+/-]+\.dds\Z')


def make_index(root):
    from PIL import Image

    originals = json.loads((root/'original-manifest.json').read_text(encoding='utf-8'))['textures']
    pack = root/'texture_pack_dds'
    entries = {}
    for original in originals:
        source = original['file']
        file = Path(source).with_suffix('.dds').as_posix()
        if not PATH_PATTERN.fullmatch(file) or file.startswith('/') or '..' in file:
            raise ValueError(f'Unsupported DDS path: {file}')
        if not (pack/file).is_file():
            raise FileNotFoundError(pack/file)
        width, height = original['width'], original['height']
        # Lightmaps store precomputed lighting. The buy-menu lookup textures
        # and normal/data maps also require the original numeric signal.
        name = original['name'].lower()
        if name.startswith(('lightmaps', 'detail/', 'detail\\')) or name.endswith('_data'):
            continue
        keys = []
        for raw in original['runtime_keys']:
            fmt, w, h, digest = raw.split('-')
            fmt = int(fmt, 16)
            if fmt in PACKED_FORMATS and (int(w), int(h)) == (width, height):
                keys.append((fmt, width, height, int(digest, 16)))
        if not original['runtime_keys']:
            # Indexed disc sources may be expanded by the guest before upload.
            # An exact match after decode is safe; resampled or recolored variants
            # keep the original texture unless an archived packed variant matches.
            with Image.open(root/'original_png'/source) as image:
                argb = image.convert('RGBA').tobytes('raw', 'BGRA')
            digest = zlib.crc32(argb) << 32 | zlib.adler32(argb)
            keys.append((0x100, width, height, digest))
        for key in keys:
            previous = entries.setdefault(key, file)
            if previous != file:
                raise ValueError(f'Ambiguous replacement: {previous} and {file}')

    output = pack/'index.tsv'
    temporary = output.with_suffix('.tmp')
    with temporary.open('w', encoding='ascii', newline='\n') as out:
        out.write('CSXB-DDS-1\n')
        for (fmt, width, height, digest), file in sorted(entries.items()):
            out.write(f'{fmt:03X} {width} {height} {digest:016X} {file}\n')
    temporary.replace(output)
    return output, len(entries), len(set(entries.values()))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('asset_root', type=Path, help='Folder containing the original manifest, PNGs and DDS pack')
    output, keys, textures = make_index(parser.parse_args().asset_root)
    print(f'{output} ({keys:,} keys for {textures:,} DDS files)')
