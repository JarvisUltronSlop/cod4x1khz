"""Extract real baseline/proposed entry functions; prepare only, never compile."""
from pathlib import Path
import hashlib
import json
import propose

ROOT = Path(__file__).resolve().parents[2]
STAGE = Path(__file__).resolve().parent

def main():
    before, after = propose.changes()
    output = []
    for label, sources in (('baseline', before), ('proposed', after)):
        header = sources[propose.FILES[0]]
        fence = sources[propose.FILES[1]]
        declaration = header[header.index('class KHZ_ServerPhysicsEntry {'):header.index('[[noreturn]] void KHZ_ServerPhysicsFatalRejection')]
        begin = fence.index('namespace khz_server_physics_fence_detail {')
        end = fence.index('void KHZ_ServerPhysicsDrainGuardRefusals() {')
        output.extend(['namespace ' + label + ' {', 'using namespace dependency;',
                       'void KHZ_ServerPhysicsDrainGuardRefusals() {}', declaration,
                       fence[begin:end], '}'])
    (STAGE / 'native_snippets.hpp').write_text('\n\n'.join(output), encoding='utf8')
    (STAGE / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.25)
project(native_physics_entry_unwind LANGUAGES CXX)
add_executable(contracts contracts.cpp)
target_compile_features(contracts PRIVATE cxx_std_20)
target_compile_options(contracts PRIVATE /W4 /UNDEBUG)
target_compile_definitions(contracts PRIVATE COD4X1KHZ_SERVER_CHECKPOINT=1 KISAK_DEDICATED=1)
''', encoding='utf8')
    result = {'prepared': True, 'compiled': False, 'production_applied': False,
              'scope': 'Actual native Physics entry/constructor/destructor/abort bodies with controlled owner/coordinator dependencies; no engine error or loaded-world proof',
              'source_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in propose.FILES},
              'snippets_sha256': hashlib.sha256((STAGE / 'native_snippets.hpp').read_bytes()).hexdigest()}
    (STAGE / 'contracts-prepared.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'prepared': True, 'compiled': False}))

if __name__ == '__main__':
    main()
