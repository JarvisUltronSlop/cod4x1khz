"""Group diagnostic native thread stack samples; counts include waits."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re


def analyze(path):
    threads=[]
    current=None
    for line in path.read_text(errors='replace').splitlines():
        match=re.fullmatch(r'thread=(\d+) samples=(\d+) failures=(\d+) cpu_ms=([\d.]+)',line)
        if match:
            current={'thread': int(match[1]), 'samples': int(match[2]), 'failures': int(match[3]),
                     'cpu_ms': float(match[4]), 'leaf': Counter(), 'inclusive': Counter()}
            threads.append(current)
        elif current is not None and line.startswith(' count='):
            fields=line.split(' | ')
            count=int(fields[0].split('=')[1])
            functions=[field.split('+')[0] for field in fields[1:]]
            if functions:
                current['leaf'][functions[0]]+=count
                for function in set(functions):
                    current['inclusive'][function]+=count
    for thread in threads:
        thread['leaf']=thread['leaf'].most_common(15)
        thread['inclusive']=thread['inclusive'].most_common(20)
    summary={'scope': 'Wall-time samples include blocked and descheduled threads. CPU milliseconds are separate per-thread execution deltas.',
             'threads': sorted(threads,key=lambda thread:-thread['cpu_ms'])}
    path.with_suffix('.analysis.json').write_text(json.dumps(summary,indent=2)+'\n')
    for thread in summary['threads']:
        if thread['cpu_ms']<100:
            continue
        print('thread',thread['thread'],'CPU ms',thread['cpu_ms'],'samples',thread['samples'])
        print('leaf',thread['leaf'][:10])
        print('inclusive',thread['inclusive'][:12])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path',type=Path)
    analyze(parser.parse_args().path)
