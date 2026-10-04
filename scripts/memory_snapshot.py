"""Read-only Windows system/process memory snapshot; never starts/stops an app."""
import argparse
import ctypes as ct
from ctypes import wintypes as wt
from datetime import datetime, timezone
import json
from pathlib import Path

from monitor import MemoryCounters


class PerformanceInfo(ct.Structure):
    _fields_=[('cb',wt.DWORD)]+[(name,ct.c_size_t) for name in
        ('commit_total','commit_limit','commit_peak','physical_total','physical_available',
         'system_cache','kernel_total','kernel_paged','kernel_nonpaged','page_size')]+[
         ('handle_count',wt.DWORD),('process_count',wt.DWORD),('thread_count',wt.DWORD)]


class ProcessEntry(ct.Structure):
    _fields_=[('size',wt.DWORD),('usage',wt.DWORD),('pid',wt.DWORD),('heap',ct.c_size_t),
             ('module',wt.DWORD),('threads',wt.DWORD),('parent',wt.DWORD),
             ('priority',wt.LONG),('flags',wt.DWORD),('name',wt.WCHAR*260)]


def capture():
    kernel=ct.WinDLL('kernel32',use_last_error=True);psapi=ct.WinDLL('psapi',use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes=[wt.DWORD,wt.DWORD];kernel.CreateToolhelp32Snapshot.restype=wt.HANDLE
    kernel.Process32FirstW.argtypes=kernel.Process32NextW.argtypes=[wt.HANDLE,ct.POINTER(ProcessEntry)]
    kernel.Process32FirstW.restype=kernel.Process32NextW.restype=wt.BOOL
    kernel.OpenProcess.argtypes=[wt.DWORD,wt.BOOL,wt.DWORD];kernel.OpenProcess.restype=wt.HANDLE
    kernel.CloseHandle.argtypes=[wt.HANDLE];kernel.CloseHandle.restype=wt.BOOL
    psapi.GetProcessMemoryInfo.argtypes=[wt.HANDLE,ct.POINTER(MemoryCounters),wt.DWORD];psapi.GetProcessMemoryInfo.restype=wt.BOOL
    psapi.GetPerformanceInfo.argtypes=[ct.POINTER(PerformanceInfo),wt.DWORD];psapi.GetPerformanceInfo.restype=wt.BOOL
    info=PerformanceInfo();info.cb=ct.sizeof(info)
    if not psapi.GetPerformanceInfo(ct.byref(info),info.cb):raise ct.WinError(ct.get_last_error())
    processes=[];unreadable=[]
    snapshot=kernel.CreateToolhelp32Snapshot(2,0)
    if snapshot==ct.c_void_p(-1).value:raise ct.WinError(ct.get_last_error())
    try:
        entry=ProcessEntry();entry.size=ct.sizeof(entry)
        found=kernel.Process32FirstW(snapshot,ct.byref(entry))
        while found:
            handle=kernel.OpenProcess(0x1000,False,entry.pid)
            if handle:
                try:
                    memory=MemoryCounters();memory.cb=ct.sizeof(memory)
                    if psapi.GetProcessMemoryInfo(handle,ct.byref(memory),memory.cb):
                        processes.append({'pid':entry.pid,'parent_pid':entry.parent,'name':entry.name,
                                          'resident_bytes':memory.rss,'private_commit_bytes':memory.private})
                    else:unreadable.append({'pid':entry.pid,'name':entry.name})
                finally:kernel.CloseHandle(handle)
            else:unreadable.append({'pid':entry.pid,'name':entry.name})
            found=kernel.Process32NextW(snapshot,ct.byref(entry))
    finally:kernel.CloseHandle(snapshot)
    processes.sort(key=lambda p:p['resident_bytes'],reverse=True)
    groups={name:{'processes':0,'resident_bytes':0,'private_commit_bytes':0} for name in ('steam','game','codex')}
    for process in processes:
        name=process['name'].lower()
        group='steam' if name in ('steam.exe','steamwebhelper.exe') else 'game' if name.startswith('cod4x1khz') else 'codex' if name in ('chatgpt.exe','codex.exe') else None
        if group:
            groups[group]['processes']+=1
            for field in ('resident_bytes','private_commit_bytes'):groups[group][field]+=process[field]
    fields=('commit_total','commit_limit','commit_peak','physical_total','physical_available','system_cache','kernel_paged','kernel_nonpaged')
    return {'captured_utc':datetime.now(timezone.utc).isoformat(),
            'scope':'Read-only memory accounting. Resident sets include shared pages and cannot be summed as unique physical memory; one snapshot does not establish a leak.',
            'system':{name+'_bytes':getattr(info,name)*info.page_size for name in fields},
            'groups':groups,'processes':processes,'unreadable_or_exited':unreadable}


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args();root=Path(__file__).resolve().parents[1];output=args.output.resolve()
    if not output.is_relative_to(root):parser.error('--output must remain inside this project')
    report=capture();output.parent.mkdir(parents=True,exist_ok=True)
    partial=output.with_name(output.name+'.part');partial.write_text(json.dumps(report,indent=2)+'\n');partial.replace(output)
    print(json.dumps({'output':str(output),'available_gib':report['system']['physical_available_bytes']/2**30,
                      'committed_gib':report['system']['commit_total_bytes']/2**30,'groups':report['groups']}))


if __name__=='__main__':main()
