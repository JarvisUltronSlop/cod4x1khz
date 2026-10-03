"""Exercise the engine's actual stock menu/control APIs over gated localhost diagnostics."""
import argparse
import hashlib
import json
from pathlib import Path
import time
from monitor import query


def inventory(port, menu):
    items, offset = [], 0
    while True:
        reply = query(port, f'khz_ui inspect {menu} {offset}', 'KHZ_UI')
        if reply.get('result') != 'inspected':
            raise RuntimeError(f'Could not inspect stock menu {menu}: {reply}')
        page = reply['items']
        items.extend(page)
        offset += 16
        if offset >= reply['item_count']:
            return items


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=28994)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    project = Path(__file__).resolve().parent.parent
    result = {'scope': 'Native stock frontend menu/control inspection; live gameplay and complete options tests are separate gates',
              'binary_sha256': hashlib.sha256((project / 'cod4x1khz.exe').read_bytes()).hexdigest(), 'cases': [], 'menus': {}}
    def check(name, passed, evidence):
        result['cases'].append({'name': name, 'passed': bool(passed), 'evidence': evidence})
    try:
        first = query(args.port, 'khz_ui list', 'KHZ_UI', 3)
        result['initial'] = first
        check('Vanilla main menu loaded', first['top_menu'] in ('main', 'main_text'), first['top_menu'])
        check('Owned menu adaptation complete', first['ui']['mode_mask'] == 127 and first['ui']['bot_rows'] == 14 and not first['ui']['failures'], first['ui'])
        names = first['menus']
        options = [name for name in names if name.startswith('options_') or name in ('controls_multi', 'main_options', 'main_controls')]
        required_options = ('options_graphics', 'options_graphics_texture', 'options_sound', 'options_voice', 'options_game', 'options_multi', 'options_look', 'options_move', 'options_shoot', 'options_misc', 'controls_multi')
        check('Stock Options inventory present', all(name in options for name in required_options), options)
        modes = [name for name in names if name.startswith('settings_quick_') and '@' not in name]
        for menu in options + modes + [name for name in ('main', 'main_text', 'single_popmenu') if name in names]:
            opened = query(args.port, 'khz_ui menu ' + menu, 'KHZ_UI')
            items = inventory(args.port, menu)
            result['menus'][menu] = {'open': opened, 'items': items}
            # Stock 'main' is an empty wrapper whose onOpen opens main_text.
            check('Stock menu opens: ' + menu, opened['result'] == 'opened' and (bool(items) or (menu == 'main' and opened['top_menu'] == 'main_text')), opened['top_menu'])
            if menu in modes:
                controls = {item['dvar']: item for item in items if item['dvar'] in ('ui_khz_botCount', 'ui_khz_botDifficulty')}
                check('Stock bot controls: ' + menu, len(controls) == 2 and all(item['type'] == 12 for item in controls.values()), controls)
                for item in controls.values():
                    # Use the same native key handler as a user, then reverse it.
                    for key in (157, 156):
                        action = query(args.port, f'khz_ui key {menu} {item["index"]} {key}', 'KHZ_UI')
                        check('Native multichoice key: ' + menu + '/' + item['dvar'] + '/' + str(key), action['result'] == 'stock action executed', action)
        query(args.port, 'khz_ui menu main_text', 'KHZ_UI')
        singleplayer = [item for item in inventory(args.port, 'main_text') if item['text'] == '@MENU_SINGLE_PLAYER']
        check('Singleplayer remains visible and disabled', len(singleplayer) == 1 and singleplayer[0]['visible'] and not singleplayer[0]['enabled'], singleplayer)
        if singleplayer:
            rejected = query(args.port, f'khz_ui activate main_text {singleplayer[0]["index"]}', 'KHZ_UI')
            check('Native Singleplayer action rejected', rejected['result'] == 'stock item disabled', rejected)
        initial_blocked = query(args.port, 'khz_ui status', 'KHZ_UI')['ui']['blocked_singleplayer_requests']
        query(args.port, 'khz_ui singleplayer', 'KHZ_UI')
        time.sleep(.05)
        blocked = query(args.port, 'khz_ui status', 'KHZ_UI')
        check('Actual Singleplayer console path blocked', blocked['ui']['blocked_singleplayer_requests'] == initial_blocked + 1 and blocked['top_menu'] == 'main_text', blocked)
        for command in ('fovscale 1.25', 'fovscale nan', 'fovscale 1', 'com_maxfps 240', 'com_maxfps 0'):
            queued = query(args.port, 'khz_ui console ' + command, 'KHZ_UI')
            time.sleep(.05)
            after = query(args.port, 'khz_ui status', 'KHZ_UI')
            expected = 1.25 if command in ('fovscale 1.25', 'fovscale nan') else 1
            passed = queued['result'] == 'queued'
            if command.startswith('fovscale'):
                passed &= abs(after['fov_scale'] - expected) < .00001
            else:
                passed &= after['render_cap'] == int(command.split()[1])
            check('Native console: ' + command, passed, after)
        result['final'] = query(args.port, 'khz_ui status', 'KHZ_UI')
        check('Bot settings restored after native keys', result['final']['ui']['bot_count'] == first['ui']['bot_count'] and result['final']['ui']['bot_difficulty'] == first['ui']['bot_difficulty'], result['final']['ui'])
    except Exception as error:
        result['error'] = str(error)
    result['passed'] = bool(result['cases']) and not result.get('error') and all(case['passed'] for case in result['cases'])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding='utf8')
    print(json.dumps({'passed': result['passed'], 'cases': len(result['cases']), 'error': result.get('error'), 'output': str(args.output)}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
