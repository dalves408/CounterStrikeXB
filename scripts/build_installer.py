"""Builds the Counter-Strike player release: the game, importer and settings program, then the
Setup and the RecompLauncher ZIP (the same steps as the Marvel vs. Capcom 2 recompilation's release).

    python scripts/build_installer.py --version 1.0.0

Output in installer/Output: CounterStrike-Setup-<version>.exe and CounterStrike-v<version>-Windows-x64.zip, each with
a .sha256. Both contain code translated from the game, so they stay out of git.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build_release'
CONFIG = 'RelWithDebInfo'
INSTALLER = ROOT / 'installer'
OUTPUT = INSTALLER / 'Output'
WIZARD_SIZES = (55, 64, 80, 96, 128)
# The hi-res texture pack the Setup offers (task "hires"): its parts' sizes and hashes, pinned
# (scripts/build_texture_payload.py), and the release they are downloaded from.
TEXTURE_PAYLOAD = INSTALLER / 'texture-payload-1.0.0.json'


def find_iscc():
    """Inno Setup 6's compiler: --iscc or ISCC_EXE if given, else a per-user install (the default for a non-admin
    install), else a machine-wide one, else ISCC on the PATH."""
    places = [os.environ.get('ISCC_EXE', '')]
    if os.environ.get('LOCALAPPDATA'):
        places.append(str(Path(os.environ['LOCALAPPDATA']) / 'Programs' / 'Inno Setup 6' / 'ISCC.exe'))
    for env in ('ProgramFiles(x86)', 'ProgramFiles'):
        if os.environ.get(env):
            places.append(str(Path(os.environ[env]) / 'Inno Setup 6' / 'ISCC.exe'))
    places.append(shutil.which('ISCC') or '')
    for place in places:
        if place and Path(place).is_file():
            return Path(place)
    sys.exit('Inno Setup 6 (ISCC.exe) was not found. Install Inno Setup 6.4 or newer, or pass --iscc <path to ISCC.exe> '
             '(or set ISCC_EXE). Looked in:\n  ' + '\n  '.join(p for p in places if p))


def run(*command):
    print('>', ' '.join(str(c) for c in command), flush=True)
    subprocess.run([str(c) for c in command], check=True, cwd=ROOT)


def set_rc_version(path, version):
    """Keep a .rc file's version resource equal to the release number."""
    parts = version.split('.')
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        sys.exit('version must look like 1.0.0')
    commas, dots = ','.join(parts + ['0']), '.'.join(parts + ['0'])
    text = path.read_text(encoding='utf-8')
    new = re.sub(r'(FILEVERSION|PRODUCTVERSION) [\d,]+', rf'\1 {commas}', text)
    new = re.sub(r'("(?:FileVersion|ProductVersion)", ")[\d.]+', rf'\g<1>{dots}', new)
    if new != text:
        path.write_text(new, encoding='utf-8')


# The wizard's pictures, in the look of the game's manual (src/cs_ui.c draws the game's own menus the same way): a
# navy blueprint page with faint gold rings, a heavy white title after a yellow notch over a gold double rule, and the
# manual's round page dial in a corner. Each is made in several sizes, one per screen scaling.
BG_TOP, BG_MID, BG_BOT = (34, 70, 112), (18, 44, 82), (8, 22, 46)
GRID, RING, GOLD, GOLD_DK = (38, 74, 116), (96, 112, 74), (226, 198, 42), (150, 132, 30)
OUTLINE, CREAM, NAVY = (6, 16, 36), (240, 232, 176), (10, 26, 56)
WIZARD_IMAGE_SIZES = ((164, 314), (192, 386), (246, 459), (273, 523), (328, 628))
WIZARD_BACK_SIZES = ((640, 480), (960, 720), (1280, 960))


def manual_font(px, bold=True):
    from PIL import ImageFont
    font = ImageFont.truetype(str(Path(os.environ.get('WINDIR', 'C:/Windows')) / 'Fonts' / 'bahnschrift.ttf'), px)
    try:
        font.set_variation_by_name('Bold' if bold else 'Regular')
    except (OSError, ValueError):
        pass
    return font


def manual_page(w, h):
    """The manual's blueprint page: a navy gradient, a grid, two faint gold rings and a sweep."""
    from PIL import ImageDraw
    img = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(img)
    mid = h * 2 // 5
    for y in range(h):
        a, b, t, n = (BG_TOP, BG_MID, y, mid) if y < mid else (BG_MID, BG_BOT, y - mid, h - mid)
        d.line([(0, y), (w, y)], fill=tuple(a[i] + (b[i] - a[i]) * t // max(n, 1) for i in range(3)))
    step = max(h // 26, 8)
    for x in range(step // 2, w, step):
        d.line([(x, 0), (x, h)], fill=GRID)
    for y in range(step // 2, h, step):
        d.line([(0, y), (w, y)], fill=GRID)
    for r in (h * 6 // 10, h * 4 // 10):
        d.ellipse([w // 2 - r, h // 2 - r, w // 2 + r, h // 2 + r], outline=RING, width=max(1, h // 320))
    d.arc([-w // 3, h // 4, w * 4 // 3, h * 2], 200, 340, fill=RING, width=max(1, h // 320))
    return img


def manual_dial(img, right=False):
    """The page dial in a bottom corner."""
    from PIL import ImageDraw
    import math
    w, h = img.size
    R = max(18, min(w, h) // 5)
    cx, cy = (w if right else 0), h
    d = ImageDraw.Draw(img)
    d.ellipse([cx - R - 3, cy - R - 3, cx + R + 3, cy + R + 3], fill=(96, 100, 100))
    d.ellipse([cx - R, cy - R, cx + R, cy + R], fill=(176, 178, 170))
    for i in (1, 2, 3):
        a = math.pi / 2 * i / 4
        dx, dy = int(R * math.cos(a)), int(R * math.sin(a))
        d.line([(cx, cy), ((cx - dx) if right else (cx + dx), cy - dy)], fill=(96, 100, 100), width=max(2, R // 18))
    r = R * 45 // 100
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(232, 222, 48))


def manual_title(img, x, y, text, px):
    """A heading as the manual sets them: heavy white with a dark outline."""
    from PIL import ImageDraw
    ImageDraw.Draw(img).text((x, y), text, font=manual_font(px), fill=(244, 246, 250),
                             stroke_width=max(1, px // 14), stroke_fill=OUTLINE)


def wizard_images():
    """The wizard's pictures (cs.iss, cs_textures.iss): the small one at the top right of every page (the game's icon
    on the manual's navy), the large one on the Welcome and Finished pages, and the page behind every page."""
    from PIL import ImageDraw
    icon = Image.open(ROOT / 'resources' / 'cs.png').convert('RGBA')
    for size in WIZARD_SIZES:
        canvas = Image.new('RGBA', (size, size), NAVY + (255,))
        canvas.alpha_composite(icon.resize((size, size), Image.LANCZOS))
        canvas.convert('RGB').save(INSTALLER / f'wizard_small_{size}.bmp')
    for w, h in WIZARD_IMAGE_SIZES:
        img = manual_page(w, h)
        d = ImageDraw.Draw(img)
        k = w / 164
        # the yellow notch and the gold double rule under the title, as on each of the manual's pages
        top = int(34 * k)
        d.polygon([(0, top), (int(16 * k), top + int(20 * k)), (0, top + int(40 * k))], fill=GOLD)
        manual_title(img, int(22 * k), top - int(4 * k), 'Counter-', int(26 * k))
        manual_title(img, int(22 * k), top + int(24 * k), 'Strike', int(26 * k))
        rule = top + int(62 * k)
        d.line([(0, rule), (w, rule)], fill=GOLD, width=max(2, int(4 * k)))
        d.line([(0, rule + int(6 * k)), (w, rule + int(6 * k))], fill=GOLD_DK, width=1)
        d.text((int(22 * k), rule + int(12 * k)), 'Xbox, on your PC', font=manual_font(int(12 * k), False), fill=CREAM)
        logo = int(84 * k)
        img.paste(icon.resize((logo, logo), Image.LANCZOS), ((w - logo) // 2, int(h * 0.62) - logo // 2),
                  icon.resize((logo, logo), Image.LANCZOS))
        manual_dial(img)
        img.save(INSTALLER / f'wizard_image_{w}.bmp')
    for w, h in WIZARD_BACK_SIZES:
        manual_page(w, h).save(INSTALLER / f'wizard_back_{w}.png')


def keep_symbols(version):
    """The exe and its .pdb, kept on this PC and never shipped: scripts/symbolize_log.py names the code offsets in a
    player's [FREEZE] or [CRASH] report with them. out/ is git-ignored."""
    release = BUILD / CONFIG
    pdb = release / 'cs2_xbox_recomp.pdb'
    if not pdb.is_file():
        sys.exit(f'{pdb} is missing: the release build makes it (CMakeLists.txt, CS_RELEASE)')
    keep = ROOT / 'out' / 'symbols' / version
    keep.mkdir(parents=True, exist_ok=True)
    for name in ('cs2_xbox_recomp.exe', 'cs2_xbox_recomp.pdb'):
        shutil.copyfile(release / name, keep / name)
    print(f'Symbols kept in {keep}', flush=True)


def write_sha256(path):
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    path.with_name(path.name + '.sha256').write_text(f'{digest}  {path.name}\n')
    return digest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version', required=True)
    parser.add_argument('--skip-build', action='store_true', help='use the programs already in build_release')
    parser.add_argument('--iscc', help="Inno Setup 6's ISCC.exe, if it is not found by itself")
    parser.add_argument('--texture-url-base', help='where Setup downloads the texture parts from '
                        '(default: the textures release on GitHub; an http://127.0.0.1 URL for tests)')
    parser.add_argument('--output', type=Path, default=OUTPUT, help='where the Setup goes (default installer/Output)')
    args = parser.parse_args()
    if args.iscc:
        os.environ['ISCC_EXE'] = args.iscc
    iscc = find_iscc()
    print(f'Inno Setup: {iscc}', flush=True)

    for rc in ('cs.rc', 'config.rc', 'insignia.rc'):
        set_rc_version(ROOT / 'resources' / rc, args.version)
    if not args.skip_build:
        run('cmake', '-S', ROOT, '-B', BUILD, '-DCS_RELEASE=ON')
        run('cmake', '--build', BUILD, '--config', CONFIG, '-j', '12',
            '--target', 'cs2_xbox_recomp', 'cs_import', 'config', 'insignia_setup', 'cs_texture_download')
    # The title update as a patch from the disc's default.xbe (scripts/tu_patch.py): installed beside the
    # game, which applies it when an install still has the disc's file, and used by cs_import.
    run(sys.executable, ROOT / 'scripts' / 'tu_patch.py', 'make', '--out', BUILD / CONFIG / 'cs_tu.xbepatch')
    keep_symbols(args.version)
    wizard_images()
    sys.path.insert(0, str(ROOT / 'scripts'))
    from build_texture_addon import RELEASE_URL_BASE, write_includes
    payload = json.loads(TEXTURE_PAYLOAD.read_text(encoding='ascii'))
    url_base = args.texture_url_base or RELEASE_URL_BASE.format(version=payload['version'])
    if not url_base.endswith('/'):
        url_base += '/'
    write_includes(payload, INSTALLER, url_base)   # git-ignored, made fresh for every build
    print(f'Texture pack {payload["version"]}: {len(payload["parts"])} parts from {url_base}', flush=True)
    args.output.mkdir(parents=True, exist_ok=True)
    run(iscc, f'/DAppVersion={args.version}', f'/DBuildDir={BUILD / CONFIG}', f'/O{args.output}', INSTALLER / 'cs.iss')
    setup = args.output / f'CounterStrike-Setup-{args.version}.exe'
    print(f'Setup: {setup}\nSHA-256: {write_sha256(setup)}')
    run(sys.executable, ROOT / 'scripts' / 'recomplauncher_manifest.py', '--version', args.version,
        '--build', BUILD / CONFIG)


if __name__ == '__main__':
    main()
