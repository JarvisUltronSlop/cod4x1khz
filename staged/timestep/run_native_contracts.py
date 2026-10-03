"""Compile extracted current/proposed native bodies against controlled dependencies.

Never writes engine inputs. Run only outside actual timing/game fixtures.
"""
from pathlib import Path
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import subprocess

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare-only',action='store_true')
    parser.add_argument('--production',action='store_true',help='compare retained Build73 baseline to actual applied engine functions')
    args=parser.parse_args()
    module_name=ROOT/'staged/timestep/proposals.py'
    spec=importlib.util.spec_from_file_location('timestep_proposals',module_name)
    proposals=importlib.util.module_from_spec(spec);spec.loader.exec_module(proposals)
    names=['engine/src/game/g_items.cpp','engine/src/game_mp/g_scr_helicopter.cpp',
      'engine/src/game/g_missile.cpp','engine/src/game/g_helicopter.cpp',
      'engine/src/game/g_scr_vehicle.cpp','engine/src/bgame/bg_misc.cpp','engine/src/game_mp/g_utils_mp.cpp']
    baseline=ROOT/'staged/timestep/before73'
    source_root=baseline if args.production else ROOT
    if args.production:
        manifest=json.loads((baseline/'manifest.json').read_text())
        if manifest.get('build')!=73 or set(manifest['native_sources'])!=set(names):raise ValueError('Invalid retained native baseline')
        for name in names:
            if hashlib.sha256((baseline/name).read_bytes()).hexdigest()!=manifest['native_sources'][name]:raise ValueError('Retained baseline changed: '+name)
    sources={name:(source_root/name).read_text(encoding='utf-8-sig') for name in names}
    changed=dict(sources)
    changed[names[0]]=proposals.proposed_items(sources[names[0]])
    changed[names[1]]=proposals.proposed_helicopter(sources[names[1]])
    changed[names[2]]=proposals.proposed_missile(sources[names[2]])
    if args.production:
        actual={name:(ROOT/name).read_text(encoding='utf-8-sig') for name in names}
        for name in names:
            if actual[name]!=changed[name]:raise ValueError('Production differs from reviewed native transform: '+name)
        changed=actual
    # Unchanged MP weapon and analytic trajectory bodies remain the real native source.
    snippet=[]
    for namespace,selected in [('current',sources),('proposed',changed)]:
        snippet += [f'namespace {namespace} {{','using namespace dependency;']
        for name,function in [(names[3],'G_VehicleFrameMsec'),(names[3],'G_VehicleFrameSeconds'),
          (names[2],'MissileStepSeconds'),(names[5],'BG_EvaluateTrajectory'),(names[6],'G_SetOrigin')]:
            snippet.append(proposals.native_function(selected[name],function))
        snippet += ['void Scr_Vehicle_Think(gentity_s* e) { ++observed.heliCalls; observed.integratedSeconds += G_VehicleFrameSeconds(); e->nextthink=level.time+G_VehicleFrameMsec(); }']
        for name,function in [(names[0],'G_RunItem'),(names[1],'Helicopter_Think'),(names[4],'VEH_UpdateWeapon'),(names[2],'JavelinRotateVelocity')]:
            snippet.append(proposals.native_function(selected[name],function))
        snippet.append('}')
    output=ROOT/'staged/timestep/results'/datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    output.mkdir(parents=True)
    (output/'native_snippets.hpp').write_text('\n\n'.join(snippet),encoding='utf-8')
    driver=ROOT/'staged/timestep/contracts_driver.cpp'
    (output/'contracts_driver.cpp').write_bytes(driver.read_bytes())
    cmake='''cmake_minimum_required(VERSION 3.25)
project(native_timestep_contracts LANGUAGES CXX)
add_executable(contracts contracts_driver.cpp)
target_compile_features(contracts PRIVATE cxx_std_20)
target_compile_options(contracts PRIVATE /W4 /UNDEBUG /fp:precise)
target_compile_definitions(contracts PRIVATE COD4X1KHZ=1 KISAK_MP=1)
set_property(TARGET contracts PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
'''
    (output/'CMakeLists.txt').write_text(cmake,encoding='utf-8')
    hash_file=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    report={'passed':False,'prepared_only':args.prepare_only,'actual_production_functions':args.production,'scope':'Extracted real function bodies with synthetic dependency fixtures; no loaded BSP/VM/native ABI/full guidance/world replay',
      'native_sources':{name:hash_file(ROOT/name) for name in names},'proposal_sha256':hash_file(module_name),
      'driver_sha256':hash_file(driver),'extracted_sha256':hash_file(output/'native_snippets.hpp'),'commands':[]}
    try:
        if not args.prepare_only:
            commands=[['cmake','-S',str(output),'-B',str(output/'build'),'-G','Visual Studio 17 2022','-A','Win32'],
              ['cmake','--build',str(output/'build'),'--config','Release','--parallel','2'],
              [str(output/'build/Release/contracts.exe')]]
            for index,command in enumerate(commands):
                log=output/f'{index}.log'
                result=subprocess.run(command,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60)
                log.write_bytes(result.stdout)
                report['commands'].append({'arguments':command,'exit_code':result.returncode,'log':str(log.relative_to(ROOT)),'log_sha256':hash_file(log)})
                if result.returncode:raise RuntimeError(f'Native contract command {index} failed: {log}')
            report['assertions']=json.loads(result.stdout.decode('utf-8').strip())
            if report['assertions'].get('passed') is not True:raise RuntimeError('Native driver did not assert PASS')
            report['binary_sha256']=hash_file(output/'build/Release/contracts.exe');report['passed']=True
    except Exception as error:
        report['error']=str(error)
    (output/'result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'passed':report['passed'],'prepared_only':args.prepare_only,'result':str(output/'result.json')}))
    return 0 if args.prepare_only or report['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
