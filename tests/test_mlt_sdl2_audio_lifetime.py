# SPDX-FileCopyrightText: 2026 Kdenlive MCP contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise real MLT consumers and SDL device status across owned lifetimes."""
from pathlib import Path
import argparse, ctypes as C, hashlib, json, os, shutil, subprocess, sys, time, uuid
parser=argparse.ArgumentParser(description="Windows native SDL2 consumer lifetime regression. Uses dummy audio and generated colors only.")
parser.add_argument('--prefix',type=Path,required=True)
parser.add_argument('--old-module',type=Path,required=True)
parser.add_argument('--new-module',type=Path,required=True)
parser.add_argument('--work-dir',type=Path,required=True)
parser.add_argument('--child',nargs=3,metavar=('MODE','CASE','HOLD'))
args=parser.parse_args()
if os.name!='nt': parser.error('This dependency regression currently supports Windows only.')
PREFIX=args.prefix.resolve(strict=True)
ROOT=args.work_dir.resolve()
if args.child is None:
    ROOT=ROOT/('sdl-lifetime-'+uuid.uuid4().hex[:12]); ROOT.mkdir(parents=True)
    for mode,module in [('old',args.old_module),('new',args.new_module)]:
        repository=ROOT/mode; repository.mkdir()
        shutil.copy2(PREFIX/'lib'/'mlt'/'mltcore.dll',repository/'mltcore.dll')
        shutil.copy2(module.resolve(strict=True),repository/'mltsdl2.dll')
    cases = [('old', 'once', 0), ('old', 'once', 1), ('new', 'once', 0), ('new', 'once', 1), ('new', 'restart', 0), ('new', 'restart', 1), ('new', 'replace', 0), ('new', 'replace', 1), ('new', 'timeout', 1), ('new', 'initfail', 0)]
    logroot=ROOT; results=[]
    for mode, case, hold in cases:
        log=logroot/f'{mode}-{case}-hold-{hold}.log'
        env=dict(os.environ, SDL_AUDIODRIVER='definitely-invalid-sdl-driver' if case == 'initfail' else 'dummy', MLT_DATA=str(PREFIX/'share'/'mlt'), MLT_PROFILES_PATH=str(PREFIX/'share'/'mlt'/'profiles'))
        command=[sys.executable,'-B',__file__,'--prefix',str(PREFIX),'--old-module',str(args.old_module.resolve()),'--new-module',str(args.new_module.resolve()),'--work-dir',str(ROOT),'--child',mode,case,str(hold)]
        startup=subprocess.STARTUPINFO(); startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow=subprocess.SW_HIDE
        with log.open('xb') as stream:
            result=subprocess.run(command,stdout=stream,stderr=subprocess.STDOUT,env=env,timeout=55,startupinfo=startup,creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode: raise RuntimeError(f'Native case failed ({result.returncode}); inspect {log}')
        lines=[json.loads(line) for line in log.read_text(encoding='utf-8').splitlines() if line.startswith('{')]
        results.append({'mode':mode,'case':case,'holdExtraAudioReference':bool(hold),'log':str(log),'last':lines[-1]})
        print(json.dumps(results[-1]), flush=True)
    manifest={'status':'passed','cases':results,'originalModuleSha256':hashlib.sha256(args.old_module.read_bytes()).hexdigest(),'patchedModuleSha256':hashlib.sha256(args.new_module.read_bytes()).hexdigest()}
    (ROOT/'report.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    raise SystemExit
mode,case,hold=args.child; hold=int(hold)
dll_dirs=[os.add_dll_directory(str(PREFIX/'bin'))]
mlt=C.CDLL(str(PREFIX/'bin'/'mlt-7.dll')); sdl=C.CDLL(str(PREFIX/'bin'/'SDL2.dll'))
p,i,b=C.c_void_p,C.c_int,C.c_char_p
specs={'mlt_factory_init':([b],p),'mlt_factory_producer':([p,b,p],p),'mlt_factory_consumer':([p,b,p],p),'mlt_profile_init':([b],p),'mlt_consumer_properties':([p],p),'mlt_producer_service':([p],p),'mlt_properties_set_int':([p,b,i],i),'mlt_consumer_connect':([p,p],i),'mlt_consumer_start':([p],i),'mlt_consumer_stop':([p],i),'mlt_consumer_is_stopped':([p],i),'mlt_consumer_close':([p],None),'mlt_producer_close':([p],None),'mlt_profile_close':([p],None),'mlt_factory_close':([],None)}
for name,(args,result) in specs.items():
    fn=getattr(mlt,name); fn.argtypes=args; fn.restype=result
for name,args,result in [('SDL_InitSubSystem',[C.c_uint],i),('SDL_QuitSubSystem',[C.c_uint],None),('SDL_WasInit',[C.c_uint],C.c_uint),('SDL_GetAudioDeviceStatus',[C.c_uint],i),('SDL_CloseAudioDevice',[C.c_uint],None),('SDL_PauseAudioDevice',[C.c_uint,i],None),('SDL_GetCurrentAudioDriver',[],b)]:
    fn=getattr(sdl,name); fn.argtypes=args; fn.restype=result
assert mlt.mlt_factory_init(os.fsencode(ROOT/mode))
def active(): return [n for n in range(1,256) if sdl.SDL_GetAudioDeviceStatus(n)!=0]
def make(profile_name):
    profile=mlt.mlt_profile_init(profile_name); assert profile
    producer=mlt.mlt_factory_producer(profile,b'color',C.cast(C.c_char_p(b'red'),p)); assert producer
    consumer=mlt.mlt_factory_consumer(profile,b'sdl2_audio',None); assert consumer
    props=mlt.mlt_consumer_properties(consumer)
    for key,value in [(b'real_time',1),(b'channels',2),(b'frequency',48000),(b'video_off',1)]: assert mlt.mlt_properties_set_int(props,key,value)==0
    assert mlt.mlt_consumer_connect(consumer,mlt.mlt_producer_service(producer))==0
    return profile,producer,consumer

def play(consumer):
    assert mlt.mlt_consumer_start(consumer)==0
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        playing=[n for n in range(1,256) if sdl.SDL_GetAudioDeviceStatus(n)==1]
        if playing: return playing
        time.sleep(.01)
    raise AssertionError('No playing dummy audio device observed')

def stop(consumer, expected_leak=False):
    assert mlt.mlt_consumer_stop(consumer)==0
    assert mlt.mlt_consumer_is_stopped(consumer)
    remaining=active()
    if expected_leak:
        assert remaining,'Baseline failed to reproduce owned-device leak with held SDL reference'
        for n in remaining: sdl.SDL_CloseAudioDevice(n)
    else:
        assert not remaining, f'Stopped consumer left live device IDs {remaining}'
    assert mlt.mlt_consumer_stop(consumer)==0
    return remaining

def close(context):
    profile,producer,consumer=context
    mlt.mlt_consumer_close(consumer); mlt.mlt_producer_close(producer); mlt.mlt_profile_close(profile)

if hold: assert sdl.SDL_InitSubSystem(0x10)==0
if case == 'initfail':
    context=make(b'dv_pal'); assert mlt.mlt_consumer_start(context[2])<0
    assert not active(); assert mlt.mlt_consumer_stop(context[2])==0; close(context)
    print(json.dumps({'phase':'clean_exit','initFailureCleanup':'passed'}),flush=True)
else:
    rounds=30 if case in ('restart','replace') else 1
    context=None
    for n in range(rounds):
        if context is None:
            context=make((b'dv_pal',b'dv_ntsc',b'atsc_720p_25')[n%3])
        playing=play(context[2]); assert len(playing)==1,playing
        if case == 'timeout':
            sdl.SDL_PauseAudioDevice(playing[0],1)
            deadline=time.monotonic()+7
            while time.monotonic()<deadline:
                live=active()
                if live and any(sdl.SDL_GetAudioDeviceStatus(device)==1 for device in live):break
                time.sleep(.02)
            else: raise AssertionError('Paused audio device was not replaced after native timeout')
            assert len(live)==1,f'Timeout reinitialization leaked old device: {live}'
            print(json.dumps({'phase':'timeout_reinitialized','activeDevices':live}),flush=True)
        leaked=stop(context[2],mode=='old' and bool(hold))
        if hold: assert sdl.SDL_WasInit(0x10),'Consumer stop closed another owner subsystem reference'
        if case != 'restart' or n==rounds-1:
            close(context); context=None
    if hold: sdl.SDL_QuitSubSystem(0x10)
    # Windows timeout compatibility path deliberately keeps one subsystem ref,
    # but must still close its callback device. Release only this child ref.
    if case == 'timeout' and sdl.SDL_WasInit(0x10): sdl.SDL_QuitSubSystem(0x10)
    assert not active()
    print(json.dumps({'phase':'clean_exit','rounds':rounds,'baselineLeakObserved':bool(leaked),'liveDevices':active(),'audioSubsystemInitialized':bool(sdl.SDL_WasInit(0x10))}),flush=True)
mlt.mlt_factory_close()
