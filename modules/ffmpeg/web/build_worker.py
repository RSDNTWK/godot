import os
import shlex
import subprocess


def build_pcm_worker(target, source, env):
    sdk = env['FFMPEG_WEB_WORKER_SDK']
    args = shlex.split(env.subst('$CXX')) + [str(source[0]), '-I' + os.path.join(sdk, 'include')]
    args += [os.path.join(sdk, 'lib', 'lib' + lib + '.a') for lib in ['avformat', 'avcodec', 'swresample', 'avutil']]
    dav1d = os.path.join(sdk, 'deps', 'lib', 'libdav1d.a')
    if os.path.isfile(dav1d):
        args.append(dav1d)
    args += ['-O2', '-sMODULARIZE=1', '-sEXPORT_NAME=GodotMediaPCMModule', '-sENVIRONMENT=worker', '-sSINGLE_FILE=1', '-sALLOW_MEMORY_GROWTH=1', '-sMAXIMUM_MEMORY=536870912', '-sABORTING_MALLOC=0', '-sFORCE_FILESYSTEM=1', '-sDYNAMIC_EXECUTION=0', '-sEXPORTED_FUNCTIONS=_malloc,_free,_godot_pcm_decode,_godot_pcm_data,_godot_pcm_size,_godot_pcm_title,_godot_pcm_error_message', '-sEXPORTED_RUNTIME_METHODS=HEAPU8,UTF8ToString', '-o', str(target[0])]
    return subprocess.call(args, env=env['ENV'])
