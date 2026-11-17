"""The RecompLauncher manifest (recomplauncher.json) for the Counter-Strike Windows release, and the SDK ZIP that carries it.

    python scripts/recomplauncher_manifest.py --version 1.0.0 [--build build_release/RelWithDebInfo] [--out installer/Output]

writes CounterStrike-v<version>-Windows-x64.zip (game, importer, settings program, recomplauncher.json) and its .sha256. Level 3 of the RecompLauncher guide (https://recomplauncher.com/sdk):
prepare (headless cs_import --prepare) and game.dumps (Redump). The Inno Setup installer stays the choice for players
who do not use the launcher.

The game relaunches itself for every map change (src/cs_launch.c), inside the window of the process the launcher
started (src/cs_frame.c), which waits for the last relaunch: the launcher sees one session, and its exit code.
"""
import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

# Redump, Microsoft Xbox, Counter-Strike (USA) (En,Fr,Es,It,Ko) (http://redump.org/disc/35835/): the image this build
# was made from, checked by CRC-32.
DUMPS = [{'name': 'Counter-Strike', 'size': 7825162240, 'crc32': '275fb9f8'}]

DATA = '{local}/CounterStrikeXB'    # %LOCALAPPDATA%\CounterStrikeXB: settings.ini, keymap.ini, SaveData, logs


def manifest():
    def setting(identifier, label, key, choices, default, help_text=None):
        row = {
            'id': identifier, 'label': label,
            'choices': [{'label': name, 'set': {key: str(value)}} for name, value in choices],
            'defaults': {key: str(default)},
        }
        if help_text:
            row['help'] = help_text
        return row

    toggle = [('Off', 0), ('On', 1)]
    settings = [
        setting('scale', 'Render scale', 'display/resolution_scale',
                [('1x (480p)', 1), ('2x (1080p)', 2), ('3x (1440p)', 3), ('4x (4K, lighter)', 4),
                 ('4.5x (4K)', 4.5), ('5x (4K, sharpest)', 5)], 1,
                'Pick your screen. 1x is the game as it was (the Xbox\'s 640x480); higher draws it sharper but needs '
                'a faster graphics card.'),
        setting('fullscreen', 'Fullscreen', 'display/fullscreen', toggle, 1,
                'Big Picture mode always starts fullscreen. F11 or Alt+Enter switches in the game.'),
        setting('stretch', 'Stretch image', 'display/stretch', [('Off (4:3)', 0), ('On (fills window)', 1)], 0),
        setting('vsync', 'VSync', 'display/vsync', toggle, 0),
        setting('frame-rate', 'Frame rate limit', 'display/frame_rate',
                [('30', 30), ('60', 60), ('72', 72)], 60,
                '72 is the game\'s own limit. The game uses it on 144 Hz or FreeSync/G-Sync monitors; elsewhere it '
                'runs at 60, the smooth choice there.'),
        setting('msaa', 'Anti-aliasing (MSAA)', 'display/msaa', [('Off', 1), ('2x', 2), ('4x', 4), ('8x', 8)], 1),
        setting('anisotropy', 'Texture filtering', 'display/anisotropy',
                [('Off', 1), ('2x', 2), ('4x', 4), ('8x', 8), ('16x', 16)], 1),
        setting('hires-textures', 'Hi-res textures', 'display/hires_textures', toggle, 1,
                'Uses the hi-res texture pack (a texture_pack folder beside the game) when it is installed.'),
        setting('mouse-aim', 'Mouse aim', 'input/mouse_aim', toggle, 0,
                'The mouse turns the view, as in the PC game. Xbox Live will not sign in while it is on: it is '
                'controller only, shared with players on Xbox and xemu. Single player and System Link keep it.'),
        setting('mouse-sensitivity', 'Mouse sensitivity', 'input/mouse_sensitivity',
                [('0.5x', 50), ('0.75x', 75), ('1x', 100), ('1.5x', 150), ('2x', 200), ('3x', 300)], 100),
    ]
    return {
        '$schema': 'https://recomplauncher.com/schema/recomplauncher.json',
        'format': 1,
        'name': 'Counter-Strike',
        'repo': 'dalves408/CounterStrikeXB',
        'game': {
            'kind': 'iso_or_folder',
            'label': 'Your Counter-Strike Xbox disc image (the USA release) or the folder with its files (the one that holds default.xbe).',
            'check': {'file': 'default.xbe', 'xdvdfs': 'default.xbe'},
            'dumps': DUMPS,
        },
        'start': {'program': ['cs2_xbox_recomp.exe']},
        'prepare': {
            'program': ['cs_import.exe'],
            'args': ['--prepare', '{game}', '{install}/game_files'],
            'label': 'Game files from your disc',
            'timeout': 900,
        },
        'config': [{
            'format': 'ini',
            'file': [{'path': DATA + '/settings.ini'}],
            'new': {key: value for row in settings for key, value in row['defaults'].items()},
        }],
        'settings': settings,
        'saves': [DATA + '/SaveData'],
        # Named places for the buttons below. 'saves' is the launcher's own name for the save folder.
        'folders': {
            'saves': [{'path': DATA + '/SaveData'}],
            'data': [{'path': DATA}],
            # The game's own log, one file per run and per map change, the last five kept (src/cs_diag.c). The launcher
            # puts the newest .log from here in the crash report a player sends.
            'logs': [{'path': DATA + '/logs'}],
        },
        'actions': [
            {'id': 'open-saves', 'label': 'Open the save folder', 'open': 'saves'},
            {'id': 'open-data', 'label': 'Open the settings folder', 'open': 'data',
             'help': 'settings.ini (the video options) and keymap.ini (your keyboard keys), also set by config.exe.'},
            {'id': 'open-logs', 'label': 'Open the logs folder', 'open': 'logs',
             'help': 'One log per run and per map change, the last five kept. Attach the newest to a problem report.'},
        ],
        'exit_codes': {
            '2': 'The disc image or folder could not be found.',
            '3': 'That file is not an Xbox disc image.',
            '4': 'That is not the supported version of Counter-Strike (the USA Xbox release).',
            '5': 'The disc image is damaged or incomplete.',
            '6': 'The game files could not be written to the install folder.',
            '7': 'The title update could not be applied to the game files.',
        },
        'looks': {'graphics': 'd3d11', 'lines': 480},
    }


def main():
    here = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version', required=True, help='release version, e.g. 1.0.0')
    parser.add_argument('--build', type=Path, default=here / 'build_release' / 'RelWithDebInfo')
    parser.add_argument('--out', type=Path, default=here / 'installer' / 'Output')
    parser.add_argument('--json-only', action='store_true', help='print recomplauncher.json and exit')
    args = parser.parse_args()
    text = json.dumps(manifest(), indent=2) + '\n'
    if args.json_only:
        sys.stdout.write(text)
        return
    args.out.mkdir(parents=True, exist_ok=True)
    archive = args.out / f'CounterStrike-v{args.version}-Windows-x64.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as package:
        for name in ('cs2_xbox_recomp.exe', 'cs_import.exe', 'config.exe', 'insignia_setup.exe', 'cs_tu.xbepatch',
                     'SDL3.dll', 'SDL3-LICENSE.txt'):
            package.write(args.build / name, name)
        package.writestr('recomplauncher.json', text)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix('.zip.sha256').write_text(f'{digest}  {archive.name}\n')
    print(f'RecompLauncher ZIP: {archive}\nSHA-256: {digest}')


if __name__ == '__main__':
    main()
