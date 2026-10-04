"""Analyze retained native Vulkan submission and presentation timestamps."""
import csv
import hashlib
import itertools
import math
from pathlib import Path
import shutil
import statistics
import time


def distribution(values):
    values=sorted(values)
    if not values:return {'samples':0}
    def percentile(p):
        at=(len(values)-1)*p; lo=math.floor(at); hi=math.ceil(at)
        return values[lo]+(values[hi]-values[lo])*(at-lo)
    return {'samples':len(values),'mean_ms':statistics.mean(values),'p50_ms':percentile(.5),
            'p95_ms':percentile(.95),'p99_ms':percentile(.99),'max_ms':values[-1]}


def analyze(path,start_us,end_us):
    path=Path(path)
    with path.open(newline='') as source:
        header=source.readline().strip()
        if not header.startswith('# bounded_capacity='):raise ValueError('Invalid native timing header')
        completed={}
        line=source.readline()
        while line.startswith('# completed_gpu='):
            serial,ms=line.strip().split('=',1)[1].split(',')
            serial,ms=int(serial),float(ms)
            if serial<=0 or not math.isfinite(ms) or ms<0 or serial in completed:raise ValueError('Invalid final GPU timestamp result')
            completed[serial]=ms;line=source.readline()
        rows=[{k:float(v) for k,v in row.items()} for row in csv.DictReader(itertools.chain([line],source))]
    serials={int(r['serial']):r for r in rows}
    if len(serials)!=len(rows):raise ValueError('Duplicate submission serial in native timing capture')
    presented=[r for r in rows if r['presentation'] and start_us/1e6<r['seconds']<=end_us/1e6]
    if not presented:raise ValueError('No actual native presentations in the measured interval')
    displays={int(r['display_id']):[] for r in presented}
    for r in rows:
        if int(r['display_id']) in displays:displays[int(r['display_id'])].append(r)
    gpu=dict(completed)
    for r in rows:
        serial=int(r['gpu_serial'])
        if serial:
            if serial in gpu and gpu[serial]!=r['gpu_ms']:raise ValueError('Conflicting GPU timestamp results')
            gpu[serial]=r['gpu_ms']
    fields=('warm_ms','prepare_ms','commands_ms','record_ms','submit_ms','acquire_ms','present_ms','recycle_ms')
    values={field:[] for field in fields}; gpu_values=[]; missing=[]
    for display,batches in displays.items():
        for field in fields:values[field].append(sum(r[field] for r in batches))
        if all(int(r['serial']) in gpu for r in batches):gpu_values.append(sum(gpu[int(r['serial'])] for r in batches))
        else:missing.append(display)
    ids=sorted(displays)
    contiguous=ids==list(range(ids[0],ids[-1]+1))
    return {'scope':'Individual displayed frames; CPU stages sum all ordered submissions for that frame. GPU timestamps are joined to their original submission serial.',
            'trace_header':header,'trace_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
            'interval_trace_clock_us':[start_us,end_us], 'retained_records':len(rows),
            'measured_frames':len(presented),'contiguous_display_ids':contiguous,
            'gpu_complete_frames':len(gpu_values),'gpu_missing_display_ids':missing,
            'frame_time':distribution([r['frame_ms'] for r in presented]),
            'gpu_work':distribution(gpu_values),'cpu_stages':{k:distribution(v) for k,v in values.items()},
            'parallel_batches':sum(r['parallel_batches'] for batches in displays.values() for r in batches),
            'parallel_draws':sum(r['parallel_draws'] for batches in displays.values() for r in batches),
            'draws':sum(r['draws'] for batches in displays.values() for r in batches),
            'timing_capture_complete':contiguous and len(gpu_values)==len(presented),
            'full_performance_acceptance_verified':False}


def export_profile(fixture,output,samples):
    path=fixture.home/'vulkan/diagnostics/frame-timings.csv'
    result=fixture.call('khz_ui console r_vkWriteProfile')
    if result.get('result')!='queued':raise ValueError('Native frame timing export was rejected')
    deadline=time.monotonic()+10
    while not path.is_file() and time.monotonic()<deadline:time.sleep(.05)
    if not path.is_file():raise ValueError('Native timing export did not complete; partial files are retained')
    saved=Path(output)/'frame-timings.csv';shutil.copy2(path,saved)
    first=samples[0]['stats']['native_graphics']['trace_clock_us']
    last=samples[-1]['stats']['native_graphics']['trace_clock_us']
    report=analyze(saved,first,last)
    report['path']=str(saved)
    return report
