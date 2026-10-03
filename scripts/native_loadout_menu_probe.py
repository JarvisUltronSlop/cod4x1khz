"""Exercise stock sidearms, perks and grenades through actual class menu actions."""
import argparse
import hashlib
import json
from pathlib import Path
import time
from monitor import query
from native_menu_probe import inventory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=28994)
    parser.add_argument('--restore-backup', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    project = Path(__file__).resolve().parent.parent
    result = {'scope': 'Actual stock frontend sidearm, attachment, perk and grenade actions and native profile fields; connected server loadouts are separate.',
              'binary_sha256': hashlib.sha256((project / 'cod4x1khz.exe').read_bytes()).hexdigest(), 'cases': [], 'selections': []}

    def check(name, passed, evidence):
        result['cases'].append({'name': name, 'passed': bool(passed), 'evidence': evidence})

    def state():
        return query(args.port, 'khz_ui status', 'KHZ_UI', 3)

    def actions(menu):
        return [item for item in inventory(args.port, menu) if item['type'] == 1 and item['visible'] and item['enabled'] and item['action']]

    def choose(menu, *, text=None, name=None, index=None):
        matches = [item for item in actions(menu) if (text is None or item['text'] == text)
                   and (name is None or item['name'] == name) and (index is None or item['index'] == index)]
        if len(matches) != 1:
            raise RuntimeError(f'Expected one native action in {menu}: {text}/{name}/{index}; found {matches}')
        reply = query(args.port, f'khz_ui activate {menu} {matches[0]["index"]}', 'KHZ_UI', 3)
        if reply['result'] != 'stock action executed':
            raise RuntimeError(f'Native menu action failed: {reply["result"]}')
        return state()['top_menu']

    def open_class():
        query(args.port, 'khz_ui menu main_text', 'KHZ_UI', 3)
        choose('main_text', text='@MPUI_CREATE_A_CLASS')
        slots = [item for item in actions('pc_cac_popup') if item['text']]
        return choose('pc_cac_popup', index=slots[0]['index'])

    def selected():
        return state()['profile']['classes'][0]

    def basic_primary():
        open_class()
        choose('menu_cac_assault', name='primary_weapon_selection')
        menu = choose('assault_popup_cac_primary', name='assault')
        menu = choose(menu, text='@WEAPON_M16')
        menu = choose(menu, text='@MPUI_NO_ATTACHMENT')
        choose(menu, text='@MPUI_NONE')

    try:
        initial = state()
        if initial['host']['owned'] or not initial['profile']['all_unlocks']:
            raise RuntimeError('Use the unlocked isolated test profile at the main menu')
        result['initial_profile'] = initial['profile']
        basic_primary()
        # Visit perk groups before sidearms. Finish perk2 with Stopping Power,
        # so the sidearm branch is the stock pistol selector rather than Overkill.
        for group, control, count in ((0, {'text': '@MPUI_PERK_1'}, 7), (1, {'text': '@MPUI_PERK_2'}, 7), (2, {'text': '@MPUI_PERK_3'}, 8)):
            open_class()
            menu = choose('menu_cac_assault', **control)
            options = [item['text'] for item in actions(menu) if item['text'].startswith('@PERKS_')]
            check(f'All stock perks in group {group + 1} are available', len(options) == count, options)
            ids = []
            for option in options:
                open_class()
                menu = choose('menu_cac_assault', **control)
                choose(menu, text=option)
                after = selected()
                ids.append(after['perk_ids'][group])
                check(f'Native perk stored: group {group + 1}/{option}', state()['top_menu'] == 'menu_cac_assault', after)
                result['selections'].append({'kind': 'perk', 'group': group, 'text': option, 'stat_id': ids[-1]})
            check(f'Each stock perk in group {group + 1} has a distinct stat', len(set(ids)) == len(options), ids)
            if group == 1:
                open_class()
                menu = choose('menu_cac_assault', **control)
                choose(menu, text='@PERKS_STOPPING_POWER')
        open_class()
        menu = choose('menu_cac_assault', text='@MPUI_SIDE_ARM1')
        weapons = [item['text'] for item in actions(menu) if item['text'].startswith('@WEAPON_')]
        check('All five stock sidearms are usable, including gold Desert Eagle', len(weapons) == 5 and '@WEAPON_DESERTEAGLEGOLD' in weapons, weapons)
        weapon_ids = []
        for weapon in weapons:
            open_class()
            menu = choose('menu_cac_assault', text='@MPUI_SIDE_ARM1')
            menu = choose(menu, text=weapon)
            attachments = ([item['text'] for item in actions(menu) if item['text'].startswith('@MPUI_')]
                           if '_attachment_popup_' in menu else ['@MPUI_NO_ATTACHMENT'] if menu == 'menu_cac_assault' else [])
            if not attachments or '@MPUI_NO_ATTACHMENT' not in attachments:
                raise RuntimeError(f'Sidearm {weapon} opened unexpected selector {menu}')
            weapon_id = selected()['secondary_id']
            weapon_ids.append(weapon_id)
            attachment_ids = []
            for attachment in attachments:
                if attachment != attachments[0]:
                    open_class()
                    menu = choose('menu_cac_assault', text='@MPUI_SIDE_ARM1')
                    menu = choose(menu, text=weapon)
                if '_attachment_popup_' in menu:
                    choose(menu, text=attachment)
                after = selected()
                attachment_ids.append(after['secondary_attachment_id'])
                check(f'Native sidearm stored: {weapon}/{attachment}', after['secondary_id'] == weapon_id and
                      bool(after['secondary']) and ((after['secondary_attachment_id'] == 0) == (attachment == '@MPUI_NO_ATTACHMENT')), after)
                result['selections'].append({'kind': 'sidearm', 'weapon': weapon, 'attachment': attachment, 'state': after})
            check(f'Each sidearm attachment has a distinct native stat: {weapon}', len(set(attachment_ids)) == len(attachments), attachment_ids)
        check('Each stock sidearm has a distinct native weapon stat', len(set(weapon_ids)) == 5, weapon_ids)
        open_class()
        menu = choose('menu_cac_assault', text='@MPUI_SPECIAL_GRENADE')
        grenades = [item['text'] for item in actions(menu) if item['text'].startswith('@WEAPON_')]
        check('All three stock special grenades are usable', len(grenades) == 3, grenades)
        grenade_ids = []
        for grenade in grenades:
            open_class()
            menu = choose('menu_cac_assault', text='@MPUI_SPECIAL_GRENADE')
            choose(menu, text=grenade)
            after = selected()
            grenade_ids.append(after['special_grenade_id'])
            check('Native special grenade stored: ' + grenade, bool(after['special_grenade']) and state()['top_menu'] == 'menu_cac_assault', after)
        check('Each special grenade has a distinct native stat', len(set(grenade_ids)) == 3, grenade_ids)
    except Exception as error:
        result['error'] = str(error)
    finally:
        try:
            query(args.port, 'khz_ui menu main_text', 'KHZ_UI', 3)
            response = query(args.port, 'khz_ui console restoreunlocks ' + args.restore_backup, 'KHZ_UI', 3)
            time.sleep(.15)
            result['restored_profile'] = state()['profile']
            if response['result'] != 'queued' or result['restored_profile'] != result.get('initial_profile'):
                result['restore_error'] = 'Native profile fields differ from the initial backup'
        except Exception as error:
            result['restore_error'] = str(error)
    result['passed'] = bool(result['cases']) and not result.get('error') and not result.get('restore_error') and all(case['passed'] for case in result['cases'])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding='utf8')
    print(json.dumps({key: result.get(key) for key in ('passed', 'error', 'restore_error')} | {'cases': len(result['cases']), 'output': str(args.output)}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
