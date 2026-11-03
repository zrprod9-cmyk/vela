"""Build and run the PC stand of the interceptor's Vela AI host (tools/student_host_test.cpp): src/student_vk.cpp with
a header generated for MODEL (gen_student_spv.py, as the console build gets it) against the reference executor
(exec.h, vela_exec) on the same recipe, bit for bit.

    python -I student_host_test.py --model MS8iaf --gen gen_student_spv.py --helpers DIR --shaders DIR
           [--gdir DIR] [--fallback S4s4P2b] [--run "-r 960x540 -o 1920x1080 -n 8 --lowmv"] [--out hosttest]

The recipe generator (--gen), the folder with the PC build helper build_pc.py (--helpers) and the shader tree with
fullscreen.vert (--shaders) are not part of this repository.
--gdir: the recipe folder (default: GDIR_ROOT/MODEL, with GDIR_ROOT from the environment).
--fallback: an fp16 recipe compiled in as SGSR3_Student_spv_fallback.h (VELA_STUDENT=fallback forces it; the stand
then expects a mismatch warning).
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
R18 = os.path.normpath(os.path.join(HERE, '..'))


def gdir_of(model, given):
    if given:
        return given
    root = os.environ.get('GDIR_ROOT', '')
    d = os.path.join(root, model)
    if root and os.path.isdir(d):
        return d
    sys.exit('no recipe folder for ' + model + ' (pass --gdir or set GDIR_ROOT)')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--model', required=True)
    ap.add_argument('--gen', required=True, help='gen_student_spv.py (not part of this repository)')
    ap.add_argument('--helpers', required=True, help='folder with build_pc.py (not part of this repository)')
    ap.add_argument('--shaders', required=True, help='shader tree with fullscreen.vert (not part of this repository)')
    ap.add_argument('--gdir', default='')
    ap.add_argument('--fallback', default='')
    ap.add_argument('--out', default='hosttest')
    ap.add_argument('--glslang', default='glslangValidator')
    ap.add_argument('--vk-include', default=os.environ.get('VK_INCLUDE', 'include'))
    ap.add_argument('--run', default='-n 8 --lowmv')
    ap.add_argument('--no-run', action='store_true')
    a = ap.parse_args()
    GEN = a.gen
    sys.path.insert(0, a.helpers)
    import build_pc as BP
    out = os.path.join(a.out, a.model)
    spv = os.path.join(out, 'spv')
    os.makedirs(spv, exist_ok=True)
    gd = gdir_of(a.model, a.gdir)
    py = sys.executable
    r = subprocess.run([py, '-I', GEN, '--model', a.model, '--gdir', gd, '--out', os.path.join(out, 'SGSR3_Student_spv.h'),
                        '--glslang', a.glslang], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stdout + r.stderr)
    print(r.stdout.strip())
    fb = os.path.join(out, 'SGSR3_Student_spv_fallback.h')
    if a.fallback:
        r = subprocess.run([py, '-I', GEN, '--model', a.fallback, '--gdir', gdir_of(a.fallback, ''), '--out', fb,
                            '--glslang', a.glslang], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stdout + r.stderr)
        print(r.stdout.strip())
    elif os.path.exists(fb):
        os.remove(fb)
    # the reference's shaders: the recipe as is, compiled as the generator compiles (glslang -V)
    jobs = [(os.path.join(a.shaders, 'fullscreen.vert'), 'fullscreen.vert')]
    for f in sorted(os.listdir(gd)):
        if f.endswith(('.comp', '.frag')):
            jobs.append((os.path.join(gd, f), 'gen_%s_%s' % (a.model, f)))
    for s, n in jobs:
        r = subprocess.run([a.glslang, '-V', s, '-o', os.path.join(spv, n + '.spv')], capture_output=True, text=True)
        if r.returncode:
            sys.exit('%s: %s%s' % (n, r.stdout, r.stderr))
    # student_vk.cpp next to the generated header (a quoted include looks next to the including file first)
    shutil.copy(os.path.join(R18, 'src', 'student_vk.cpp'), os.path.join(out, 'student_vk.cpp'))
    env = BP.vc_env()
    lib = BP.import_lib(env, out)
    exe = os.path.join(out, 'student_host_test.exe')
    cmd = [BP.exe('cl', env), '/nologo', '/std:c++17', '/O2', '/EHsc', '/W3', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS',
           '/DUNICODE', '/D_UNICODE', '/wd4244', '/wd4267', '/wd4305', '/wd4996', '/I' + a.vk_include,
           '/I' + os.path.join(R18, 'src'), os.path.join(R18, 'tools', 'student_host_test.cpp'),
           os.path.join(out, 'student_vk.cpp'), '/Fo' + out + '\\', '/Fe' + exe, '/link', lib, 'ole32.lib']
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stdout[-8000:] + r.stderr[-3000:])
    print('built', exe)
    if a.no_run:
        return
    plan = os.path.join(gd, a.model + '.plan')
    env2 = dict(os.environ, INTERCEPT_LOG=os.path.join(out, 'host.log'))
    r = subprocess.run([exe, spv, plan] + a.run.split(), env=env2, capture_output=True, text=True)
    print(r.stdout[-6000:], r.stderr[-3000:])
    sys.exit(r.returncode)


if __name__ == '__main__':
    main()
