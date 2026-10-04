"""Replace fixed runtime sizeof assertions with measured, checked x86/x64 sizes.

Only unambiguous records whose x86 compiler size matches the original assertion
are eligible. This does not change any disk schema or approve runtime code that
assumes the old size. Generation is read-only unless --apply is supplied.
"""
import argparse
from pathlib import Path
import json
import re

def sizes(path):
    result={}
    for record in json.loads(path.read_text())['types']:
        if record['tag'] == 11: result.setdefault(record['name'],set()).add(record['bytes'])
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disk',type=Path);parser.add_argument('native',type=Path)
    parser.add_argument('report',type=Path);parser.add_argument('--apply',action='store_true')
    args=parser.parse_args()
    disk,native=sizes(args.disk),sizes(args.native)
    root=Path(__file__).resolve().parent.parent
    pattern=r'(static_assert\(sizeof\((?:struct |union )?([\w:]+)\)\s*==\s*)(0x[\dA-Fa-f]+|\d+)(\))'
    changes=[]
    for header in (root/'engine/src').rglob('*.h'):
        if header.name.endswith('.generated.h'): continue
        source=header.read_bytes().decode('latin1')
        def replace(match):
            name=match[2];old=int(match[3],0);a,b=disk.get(name),native.get(name)
            if a != {old} or not b or len(b)!=1 or b==a: return match[0]
            value=next(iter(b))
            changes.append({'file':str(header.relative_to(root)), 'type':name,'wire':old,'native':value})
            return f'{match[1]}(sizeof(void*) == 8 ? {value} : {old}){match[4]}'
        result=re.sub(pattern,replace,source)
        conditional=r'(static_assert\(sizeof\((?:struct |union )?([\w:]+)\)\s*==\s*)\(sizeof\(void\*\) == 8 \? (\d+) : (\d+)\)(\))'
        def refresh(match):
            name=match[2];old=int(match[4]);a,b=disk.get(name),native.get(name)
            if a != {old} or not b or len(b)!=1: return match[0]
            value=next(iter(b))
            if value == int(match[3]): return match[0]
            changes.append({'file':str(header.relative_to(root)), 'type':name,'wire':old,'native':value})
            return f'{match[1]}(sizeof(void*) == 8 ? {value} : {old}){match[5]}'
        result=re.sub(conditional,refresh,result)
        if args.apply and result!=source: header.write_bytes(result.encode('latin1'))
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps({'applied':args.apply,'assertions':changes},indent=2)+'\n')
    print(f'{"Updated" if args.apply else "Measured"} {len(changes)} runtime layout assertions; disk records unchanged')

if __name__ == '__main__': main()
