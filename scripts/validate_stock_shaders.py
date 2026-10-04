"""Translate and link private stock shader captures using pinned native tools."""
import argparse
import json
from pathlib import Path
import subprocess

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inventory', type=Path)
    args=parser.parse_args()
    root=Path(__file__).resolve().parent.parent
    home=args.inventory/'shaders'
    tools=root/'runtime/tools/Release'
    parse=subprocess.run([str(tools/'khz_vulkan_shader_inventory.exe'),str(home),str(args.inventory/'translation.json')])
    passes=[json.loads(line) for line in (home/'passes.jsonl').read_text().splitlines()]
    # MojoShader's linker distinguishes integer BYTE4/SHORT inputs from float
    # and normalized inputs. Packed normalized/ushort inputs remain float4 at
    # this stage; their GPU format and default W are separate material gates.
    mojo={0:0,1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:4,9:8,10:9,11:8,12:9,13:5,14:9,15:10,16:11}
    rows=[]
    for entry in passes:
        attributes=[(e[3],e[4],mojo[e[2]]) for e in entry['elements']]
        rows.append(' '.join([entry['vertex'] or '-', entry['pixel'] or '-', str(len(attributes)),
                             *[str(n) for a in attributes for n in a]]))
    manifest=args.inventory/'pairs.txt';manifest.write_text('\n'.join(rows)+'\n')
    link=subprocess.run([str(tools/'khz_vulkan_shader_link.exe'),str(home),str(manifest),str(args.inventory/'linkage.json')])
    raise SystemExit(1 if parse.returncode or link.returncode else 0)

if __name__=='__main__': main()
