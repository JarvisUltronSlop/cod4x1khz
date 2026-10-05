"""Recover an owned benchmark join through its inspected stock class control."""


def choose_assault(call, ui, state, evidence=None):
    if ui.get('top_menu') != 'changeclass' or state.get('connection') != 9:
        return None
    for offset in range(0, 512, 16):
        menu = call(f'khz_ui inspect changeclass {offset}')
        if evidence is not None:
            evidence.append({'inspect': menu})
        # Stock localization can label this @CLASS_CLASS1 instead of Assault.
        # Identify the actual compiled response, not a translated display label.
        matches = [item for item in menu.get('items', []) if item.get('visible') and item.get('enabled') and item.get('action')]
        for item in matches:
            details = call(f"khz_ui details changeclass {item['index']}")
            if evidence is not None:
                evidence.append({'item': item, 'details': details})
            if 'assault_mp' in details.get('action', ''):
                reply = call(f"khz_ui activate changeclass {item['index']}")
                return {'item': item, 'details': details, 'reply': reply}
        if offset + 16 >= menu.get('item_count', 0):
            return None
    return None
