"""Create a compiler-layout inspection TU; never compile this TU into the game.

The real engine retains its ABI assertions. This inspection TU suppresses those
assertions only to measure the new native layouts before implementing disk/native
conversion. Its generated executable contains no engine runtime implementation.
"""
import argparse
from pathlib import Path
import re

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--headers', type=Path, help='Optional inspection-only header mirror that forces PDB type emission')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    if args.headers:
        for header in (root/'engine/src').rglob('*.h'):
            text=header.read_text(encoding='latin1')
            pattern=r'(static_assert\(sizeof\(((?:struct |union )?[\w:]+)\)\s*==[^;\n]*\);)'
            identity=re.sub(r'\W','_',str(header.relative_to(root/'engine/src')))
            counter=[0]
            def force(match):
                counter[0]+=1
                return match[1]+f'\n__declspec(dllexport) {match[2]}* khz_layout_{identity}_{counter[0]} = nullptr;'
            if not header.name.endswith('.generated.h'):
                text=re.sub(pattern,force,text)
            target=args.headers/header.relative_to(root/'engine/src')
            target.parent.mkdir(parents=True,exist_ok=True); target.write_text(text,encoding='latin1')
    source = (root/'engine/src/database/db_load.cpp').read_text()
    includes = source[:source.index('// Static Prototypes')]
    includes = includes.replace('#include "database.h"', '#include <database/database.h>')
    includes = re.sub(r'#if defined\(COD4X1KHZ\).*?#endif', '', includes, flags=re.S)
    includes += '\n'.join(f'#include <{name}>' for name in (
        'aim_assist/aim_assist.h', 'EffectsCore/fx_system.h', 'devgui/devgui.h',
        'game_mp/g_main_mp.h', 'gfx_d3d/r_dobj_skin.h', 'groupvoice/directsound.h',
        'script/scr_vm.h', 'script/scr_compiler.h', 'script/scr_debugger.h',
        'script/scr_evaluate.h', 'script/scr_memorytree.h', 'script/scr_parser.h',
        'script/scr_animtree.h', 'script/scr_const.h', 'script/scr_main.h',
        'common/brush.h', 'game/game_public.h', 'game_mp/g_public_mp.h',
        'universal/q_parse.h'))+'\n'
    start = source.index('struct DynEntityServer')
    stop = source.index('void __cdecl Load_byte(bool atStreamStart)\n{', start)
    declarations = source[start:stop]
    # The production loader's converter includes belong after the inspection
    # macro is removed, so every generated native-offset assertion is active.
    declarations = declarations.split('#include "asset_native_stream.h"')[0]
    result = '''// GENERATED METADATA INSPECTION ONLY. Never link into engine targets.
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <vector>
#include <string>
#include <iostream>
#include <format>
#define static_assert(...)
'''+includes+'\nscr_const_t scr_const{};\n'+declarations+'''
#undef static_assert
#if defined(KHZ_VERIFY_ASSET_SCHEMAS)
#include <database/asset_schemas.generated.h>
#endif
int main() {
    std::printf("Metadata probe only: %zu-bit native pointers, GfxWorld=%zu, XAssetList=%zu\\n",
        sizeof(void*)*8, sizeof(GfxWorld), sizeof(XAssetList));
    return 0;
}
'''
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result)

if __name__ == '__main__':
    main()
