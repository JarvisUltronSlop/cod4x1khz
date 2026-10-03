"""Choose stock weapons/attachments/camos through native Create-a-Class actions."""
import argparse
import hashlib
import json
from pathlib import Path
import time
from monitor import query
from native_menu_probe import inventory


GROUPS = (('assault', 7), ('SMG', 5), ('LMG', 3), ('shotgun', 2), ('sniper', 5))
STANDARD_CAMOS = ('@MPUI_NONE', '@MPUI_DESERT', '@MPUI_WOODLAND', '@MPUI_DIGITAL', '@MPUI_RED_TIGER', '@MPUI_BLUE_TIGER')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=28994)
    parser.add_argument('--restore-backup', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    project = Path(__file__).resolve().parent.parent
    result = {'scope': 'Actual stock frontend item selections and native profile class fields. Server spawn/loadout tests are separate.',
              'binary_sha256': hashlib.sha256((project / 'cod4x1khz.exe').read_bytes()).hexdigest(), 'cases': [], 'weapons': []}

    def check(name, passed, evidence):
        result['cases'].append({'name': name, 'passed': bool(passed), 'evidence': evidence})

    def invoke(menu, item):
        response = query(args.port, f'khz_ui activate {menu} {item["index"]}', 'KHZ_UI', 3)
        if response['result'] != 'stock action executed':
            raise RuntimeError(f'Native {menu}/{item["text"]} action failed: {response}')

    def visible_actions(menu):
        return [item for item in inventory(args.port, menu) if item['type'] == 1 and item['visible'] and item['enabled'] and item['action']]

    def top():
        return query(args.port, 'khz_ui status', 'KHZ_UI', 3)['top_menu']

    def choose(menu, *, text=None, name=None):
        items = [item for item in visible_actions(menu) if (text is None or item['text'] == text) and (name is None or item['name'] == name)]
        if len(items) != 1:
            raise RuntimeError(f'Expected one usable native control in {menu}: text={text}, name={name}, found={items}')
        invoke(menu, items[0])

    def open_group(group):
        query(args.port, 'khz_ui menu main_text', 'KHZ_UI', 3)
        choose('main_text', text='@MPUI_CREATE_A_CLASS')
        slots = [item for item in visible_actions('pc_cac_popup') if item['text']]
        invoke('pc_cac_popup', slots[0])
        choose('menu_cac_assault', name='primary_weapon_selection')
        choose('assault_popup_cac_primary', name=group)
        return top()

    def selected():
        return query(args.port, 'khz_ui status', 'KHZ_UI', 3)['profile']['classes'][0]

    try:
        initial = query(args.port, 'khz_ui status', 'KHZ_UI', 3)
        if not initial['profile']['all_unlocks'] or initial['host']['owned']:
            raise RuntimeError('Use an unlocked isolated test profile at the main menu')
        result['initial_profile'] = initial['profile']
        for group, count in GROUPS:
            menu = open_group(group)
            weapons = [item['text'] for item in visible_actions(menu) if item['text'].startswith('@WEAPON_')]
            check('Every stock weapon usable in ' + group, len(weapons) == count, weapons)
            for weapon in weapons:
                menu = open_group(group)
                choose(menu, text=weapon)
                attachment_menu = top()
                has_attachment_selector = '_attachment_popup_' in attachment_menu
                attachments = ([item['text'] for item in visible_actions(attachment_menu) if item['text'].startswith('@MPUI_')]
                               if has_attachment_selector else ['@MPUI_NO_ATTACHMENT'] if '_cac_camo' in attachment_menu else [])
                if '@MPUI_NO_ATTACHMENT' not in attachments:
                    raise RuntimeError(f'{weapon} did not open its stock attachment selector: {attachment_menu}')
                initial_id = selected()['primary_id']
                observed_ids = []
                for attachment in attachments:
                    if attachment != attachments[0]:
                        menu = open_group(group)
                        choose(menu, text=weapon)
                    if has_attachment_selector:
                        choose(attachment_menu, text=attachment)
                    camo_menu = top()
                    camo_options = [item['text'] for item in visible_actions(camo_menu) if item['text'].startswith('@MPUI_')]
                    check('All stock standard camos usable: ' + weapon + '/' + attachment, all(camo in camo_options for camo in STANDARD_CAMOS), camo_options)
                    choose(camo_menu, text='@MPUI_NONE')
                    after = selected()
                    observed_ids.append(after['attachment_id'])
                    check('Native class selection stored: ' + weapon + '/' + attachment,
                          top() == 'menu_cac_assault' and after['primary_id'] == initial_id and bool(after['primary'])
                          and after['camo_id'] == 0 and ((after['attachment_id'] == 0) == (attachment == '@MPUI_NO_ATTACHMENT')), after)
                check('Each supported attachment has a distinct native selection: ' + weapon, len(set(observed_ids)) == len(attachments), observed_ids)
                menu = open_group(group)
                choose(menu, text=weapon)
                if has_attachment_selector:
                    choose(top(), text='@MPUI_NO_ATTACHMENT')
                camo_menu = top()
                camos = [item['text'] for item in visible_actions(camo_menu) if item['text'].startswith('@MPUI_')]
                camo_ids = []
                for camo in camos:
                    if camo != camos[0]:
                        menu = open_group(group)
                        choose(menu, text=weapon)
                        if has_attachment_selector:
                            choose(top(), text='@MPUI_NO_ATTACHMENT')
                    choose(camo_menu, text=camo)
                    after = selected()
                    camo_ids.append(after['camo_id'])
                    check('Native camo selection stored: ' + weapon + '/' + camo,
                          after['primary_id'] == initial_id and ((after['camo_id'] == 0) == (camo == '@MPUI_NONE')), after)
                check('Every exposed camo has a distinct native selection: ' + weapon, len(set(camo_ids)) == len(camos), camo_ids)
                result['weapons'].append({'weapon': weapon, 'attachments': attachments, 'camos': camos, 'primary_id': initial_id})
    except Exception as error:
        result['error'] = str(error)
    finally:
        try:
            query(args.port, 'khz_ui menu main_text', 'KHZ_UI', 3)
            response = query(args.port, 'khz_ui console restoreunlocks ' + args.restore_backup, 'KHZ_UI', 3)
            time.sleep(.15)
            result['restore'] = query(args.port, 'khz_ui status', 'KHZ_UI', 3)['profile']
            if response['result'] != 'queued' or result['restore'] != result.get('initial_profile'):
                result['restore_error'] = 'Native profile fields did not match the initial saved profile'
        except Exception as error:
            result['restore_error'] = str(error)
    result['passed'] = len(result['weapons']) == 22 and not result.get('error') and not result.get('restore_error') and all(case['passed'] for case in result['cases'])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding='utf8')
    print(json.dumps({'passed': result['passed'], 'cases': len(result['cases']), 'weapons': len(result['weapons']), 'error': result.get('error'), 'restore_error': result.get('restore_error'), 'output': str(args.output)}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
