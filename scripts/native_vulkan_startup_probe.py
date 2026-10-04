"""Run an owned, isolated native renderer startup diagnostic; restore runtime.

The accepted root binary and interactive profile are preserved. This test is a
debugger/stock-menu diagnostic, not the Vulkan visual or performance gate.
"""
import argparse
from datetime import datetime,timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time
import re
import socket
import threading
from native_fov_probe import interactive_preferences
from monitor import query

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--api',choices=['vulkan','d3d9'],default='vulkan');parser.add_argument('--host',action='store_true',help='Request a private bot match under the owned debugger');args=parser.parse_args()
    root=Path(__file__).resolve().parent.parent
    output=root/'results'/('vulkan-startup-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S'))
    output.mkdir();home=output/'home';home.mkdir()
    if args.host:
        profiles=home/'players/profiles';(profiles/'khz').mkdir(parents=True)
        (profiles/'active.txt').write_text('khz\n')
        (profiles/'khz/config_mp.cfg').write_text('seta com_maxfps "125"\nseta r_vsync "0"\n')
    source=root/'bin/Release/cod4x1khz.exe';runtime=root/'runtime/cod4x1khz.exe';saved=output/'preserved-runtime-client.exe'
    before=interactive_preferences(root);shutil.copy2(runtime,saved)
    tested_hash=hashlib.sha256(source.read_bytes()).hexdigest()
    report={'scope':__doc__,'api':args.api,'tested_sha256':tested_hash,'root_sha256':hashlib.sha256((root/'cod4x1khz.exe').read_bytes()).hexdigest()}
    stopped=threading.Event();host_thread=None
    def request_host():
        deadline=time.monotonic()+30
        while not stopped.is_set() and time.monotonic()<deadline:
            try:
                log=(output/'debug.log').read_text();pid=re.search(r'Owned client PID=(\d+)',log)
                if pid:
                    stats=query(28994,'khz_stats','KHZ_JSON',.5)
                    if stats.get('process_id')!=int(pid[1]):raise RuntimeError('Owned debug endpoint PID differs')
                    ui=query(28994,'khz_ui status','KHZ_UI',.5)
                    if ui.get('top_menu') in ('main','main_text'):
                        report['host_request']=query(28994,'khz_ui host mp_killhouse dm 15 1','KHZ_UI',1)
                        return
            except (FileNotFoundError,TimeoutError,OSError):pass
            except Exception as error:report['host_request_error']=str(error);return
            stopped.wait(.1)
    try:
        shutil.copy2(source,runtime)
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as port:port.bind(('127.0.0.1',28994))
        extra=' +set khz_benchmark 1 +set khz_test_controls 1 +set com_introPlayed 1' if args.host else ''
        command=[str(root/'build/Release/Release/khz_launch_probe.exe'),str(runtime),str(output),str(root/'bin/Release'),
                 f'+set r_graphicsApi {args.api} +set r_vkValidation 1 +set r_vsync 0 +set com_maxfps 125 +set r_mode 640x480 +set fs_homepath "{home}" +set logfile 2'+extra]
        env=dict(__import__('os').environ);env['VK_LAYER_PATH']=str(root/'.deps/vulkan/layers/Release')
        if args.host:env['KHZ_PROBE_SECONDS']='40'
        if args.host:host_thread=threading.Thread(target=request_host,daemon=True);host_thread.start()
        with (output/'debug.log').open('w') as log:
            result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,env=env,timeout=55 if args.host else 45)
        report['debugger_exit_code']=result.returncode
        debug=(output/'debug.log').read_text();print(debug[-10000:])
        report['graphics_failures']=[line for line in debug.splitlines() if 'Native Vulkan graphics failure:' in line or 'Vulkan validation:' in line or 'bad allocation' in line]
        device=home/'vulkan/diagnostics/device.json'
        report['native_device']=json.loads(device.read_text()) if device.is_file() else None
        report['capture_present']=(home/'vulkan/diagnostics/menu-12.bgra').is_file()
        report['diagnostic_passed']=result.returncode==0 and not report['graphics_failures']
        if args.api=='vulkan':
            report['diagnostic_passed'] &= bool(report['native_device'] and report['capture_present'] and
                report['native_device'].get('validation_enabled') and report['native_device'].get('validation_errors')==0)
        if args.host:
            report['diagnostic_passed'] &= report.get('host_request',{}).get('result')=='start requested'
            report['multiplayer_rendering_verified']=False
    finally:
        stopped.set()
        if host_thread:host_thread.join(2)
        if hashlib.sha256(runtime.read_bytes()).hexdigest()==tested_hash:
            for attempt in range(25):
                try:shutil.copy2(saved,runtime);break
                except PermissionError:
                    if attempt==24:raise
                    time.sleep(.2)
            report['runtime_restored']=hashlib.sha256(runtime.read_bytes()).digest()==hashlib.sha256(saved.read_bytes()).digest()
        else:report['runtime_restored']=False
        report['interactive_preferences_unchanged']=interactive_preferences(root)==before
        (output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
        print(output)
    raise SystemExit(0 if report.get('diagnostic_passed') and report['runtime_restored'] and report['interactive_preferences_unchanged'] else 1)

if __name__=='__main__':main()
