#!/usr/bin/env python3
"""The TAS unit's own tests (finding 285), on the host, without the game.

Builds `port/hle/tas.c` into a small stub host and drives it through the order the game itself
uses, one field at a time:

    tas_frame_advance_wait()   kernel.c, start of vsync(): the online gate and the pause
    ++kernel_vsync_count       kernel.c, right after
    tas_pad()                  pads_read(): only when a movie is playing
    tas_capture()              pads_read(): the pad this game consumed
    tas_vsync()                kernel.c, end of vsync(): writes the recorded field

That order is the point: a test that calls the helpers directly misses a gate or a pause that is
wrong where the game enters it, which is exactly the fault this suite was written for.

    python port/tools/tas_selftest.py            # every check

Needs a C compiler and nothing else: cc, gcc or clang, or DHD_CC. Two of the checks link a
fault-injection build with GNU ld's --wrap (`allocation failure` and `a failed field write`); a
toolchain without it skips them and says so. No game, no GPU, no disc, no network.
"""
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(HERE, 'tas_selftest')
BUILD = os.path.join(ROOT, 'build', 'tas-selftest')
WORK = os.path.join(BUILD, 'run')
INCLUDES = ['-I' + os.path.join(ROOT, 'port', 'hle'), '-I' + os.path.join(ROOT, 'port', 'include'),
            '-I' + os.path.join(ROOT, 'port', 'host')]
CFLAGS = ['-std=gnu99', '-Wall', '-Wextra', '-Wno-unused-parameter']

results = []          # (name, ok)   ok: True, False, or None for skipped


def say(msg):
    print(msg, flush=True)


def check(name, ok, detail=''):
    results.append((name, ok))
    if ok is None:
        say('SKIP  ' + name)
    elif ok:
        say('PASS  ' + name)
    else:
        say('FAIL  ' + name)
        if detail:
            say('      ' + detail.replace('\n', '\n      ').strip())


def find_cc():
    want = os.environ.get('DHD_CC')
    names = [want] if want else ['cc', 'gcc', 'clang']
    for n in names:
        if not n:
            continue
        try:
            p = subprocess.run([n, '--version'], capture_output=True, text=True)
        except OSError:
            continue
        if p.returncode == 0:
            return n
    return None


PAD_TAIL = ' 0000 128 128 128 128 0000 128 128 128 128 0000 128 128 128 128'
MOVIE = ('# DHDR TAS movie v1\n'
         '1 4000 200 55 128 128' + PAD_TAIL + '\n'
         '2 0080 129 130 131 132' + PAD_TAIL + '\n')


def write(name, text):
    path = os.path.join(WORK, name)
    with open(path, 'w') as f:
        f.write(text)
    return path


def run(binary, args, env=None, timeout=20):
    e = dict(os.environ)
    for k in list(e):
        if k.startswith('DHD_TAS_'):
            del e[k]
    e.update(env or {})
    p = subprocess.run([binary] + args, capture_output=True, text=True, env=e, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def run_blocking(binary, args, env, timeout=2.0):
    """1 when the game was still held at the timeout, which for these scenarios is a hang."""
    e = dict(os.environ)
    for k in list(e):
        if k.startswith('DHD_TAS_'):
            del e[k]
    e.update(env)
    p = subprocess.Popen([binary] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, env=e)
    try:
        out, err = p.communicate(timeout=timeout)
        return 0, out, err
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
        return 1, out, err


def pads(out):
    return [line.split('pad=')[1].strip() for line in out.splitlines() if 'pad=' in line]


def main():
    cc = find_cc()
    if not cc:
        say('tas selftest: skipped, no C compiler (set DHD_CC to one)')
        return 0
    if os.path.isdir(BUILD):
        shutil.rmtree(BUILD)
    os.makedirs(WORK)
    tas = os.path.join(ROOT, 'port', 'hle', 'tas.c')
    harness = os.path.join(SRC, 'test_tas.c')
    plain = os.path.join(BUILD, 'test_tas')
    cmd = [cc] + CFLAGS + INCLUDES + ['-o', plain, harness, tas]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        say('tas selftest: cannot build the harness:\n' + p.stdout + p.stderr)
        return 1

    fault = {}
    for name, extra, ld in (('nomem', 'failmalloc.c', '--wrap=realloc'),
                            ('flush', 'failflush.c', '--wrap=fflush')):
        out = os.path.join(BUILD, 'test_tas_' + name)
        p = subprocess.run([cc] + CFLAGS + INCLUDES + ['-Wl,' + ld, '-o', out, harness,
                                                      os.path.join(SRC, extra), tas],
                           capture_output=True, text=True)
        fault[name] = out if p.returncode == 0 else None

    # 1. offline playback survives the first vsync and applies the movie's field
    movie = write('movie.txt', MOVIE)
    rc, out, err = run(plain, ['play3'], {'DHD_TAS_PLAY': movie})
    check('offline playback survives the first vsync', 'f playing=1' in out, out + err)
    check('offline playback applies field 1', pads(out)[:1] == ['4000 200 55'], str(pads(out)))
    check('offline playback applies field 2', pads(out)[1:2] == ['0080 129 130'], str(pads(out)))
    check('offline playback past the movie is neutral', pads(out)[2:3] == ['0000 128 128'], str(pads(out)))
    check('offline playback says it is playing', 'playing 2 field(s)' in err, err)

    # 2/3. offline recording stays active after initialization and produces fields
    rec = os.path.join(WORK, 'rec.txt')
    rc, out, err = run(plain, ['rec3'], {'DHD_TAS_RECORD': rec})
    check('offline recording active after init', 'f playing=0 recording=1' in out, out + err)
    body = [l for l in open(rec).read().splitlines() if l and not l.startswith('#')]
    check('offline recording produces a field a field', len(body) == 3, str(body))
    check('offline recording wrote fields 1-3', [l.split()[0] for l in body] == ['1', '2', '3'], str(body))

    # 4. offline playback applies what was recorded
    rc, out, err = run(plain, ['play3'], {'DHD_TAS_PLAY': rec})
    played = pads(out)
    recorded = [' '.join(l.split()[1:4]) for l in body]
    check('offline playback reproduces the recording', played[:3] == recorded,
          str(played) + ' vs ' + str(recorded))

    # 4b. player 1 only: pads 2-4 keep their own sources, and are not recorded
    rc, out, err = run(plain, ['other-pads'], {'DHD_TAS_PLAY': movie})
    check('a movie does not answer for pads 2-4', 'other_pad_taken=0' in out and 'untouched=1' in out,
          out + err)
    rec2 = os.path.join(WORK, 'rec-other.txt')
    rc, out, err = run(plain, ['record-other'], {'DHD_TAS_RECORD': rec2})
    rec2line = [l.split() for l in open(rec2).read().splitlines() if l and not l.startswith('#')]
    check('player 1 is the pad that is recorded',
          len(rec2line) == 1 and rec2line[0][:6] == ['1', '4000', '99', '128', '128', '128'], str(rec2line))
    check('a second pad is not recorded', len(rec2line) == 1 and rec2line[0][6:] ==
          ['0000', '128', '128', '128', '128'] * 3, str(rec2line))

    # 4c. the clock: a paced run is not repeatable, and the TAS says so once
    rc, out, err = run(plain, ['clock'], {'DHD_TAS_PLAY': movie, 'STUB_DELTA_CLOCK': '0'})
    check('fixed-step clock: no warning', 'not reproducible' not in err, err)
    check('fixed-step clock: the movie plays', 'after playing=1' in out, out + err)
    rc, out, err = run(plain, ['clock'], {'DHD_TAS_PLAY': movie, 'STUB_DELTA_CLOCK': '1'})
    check('wall-clock steps: the run is called not reproducible', 'not reproducible' in err, err)
    check('wall-clock steps: DHD_FRAME_CLOCK is named', 'DHD_FRAME_CLOCK=1' in err, err)
    check('wall-clock steps: the movie still plays', 'after playing=1' in out, out + err)
    rc, out, err = run(plain, ['clock'], {'STUB_DELTA_CLOCK': '1'})
    check('no movie, no warning', 'not reproducible' not in err, err)

    # 4d. a pause is host time: the game clock is rebased when it releases
    stepr = os.path.join(WORK, 'r.cmd')
    write('r.cmd', 'step')
    hung, out, err = run_blocking(plain, ['paused-then-online'], {'DHD_TAS_PAUSE': '1', 'DHD_TAS_STEP': stepr})
    check('a released pause rebases the game clock once',
          'paused playing=0 recording=0 paused=1 pad_calls=0 rebases=1' in out,
          out + err)
    hung, out, err = run_blocking(plain, ['nopause'], {})
    check('a run that never pauses never rebases', 'rebases=0' in out, out + err)

    # 5. online playback refusal, before any pad of the field is consumed
    rc, out, err = run(plain, ['online-play'], {'DHD_TAS_PLAY': movie})
    check('online: playback refused', 'online1 playing=0' in out, out + err)
    check('online: no pad taken from the movie', 'pad_calls=0' in out, out)
    check('online: the controller supplies the field', 'pad=4400 200 55' in out, out)
    check('online: refusal is explained', 'online session: playback off' in err, err)

    # 6. online recording refusal and file closure, keeping what was written
    recref = os.path.join(WORK, 'rec-online.txt')
    rc, out, err = run(plain, ['online-rec'], {'DHD_TAS_RECORD': recref})
    check('online: recording refused', 'online playing=0 recording=0' in out, out + err)
    check('online: the file is closed with what it had',
          'recording off (TAS is offline only): 0 field(s) kept' in err, err)
    check('online: no field reached the file',
          not [l for l in open(recref).read().splitlines() if l and not l.startswith('#')])

    # 7. a pause configured for an online session never holds it
    stepa = os.path.join(WORK, 'a.cmd')
    hung, out, err = run_blocking(plain, ['online-pause-env'], {'DHD_TAS_PAUSE': '1', 'DHD_TAS_STEP': stepa})
    check('online: DHD_TAS_PAUSE does not block', (not hung) and 'FIELD_OK' in out, out + err)
    check('online: the pause is dropped', 'after playing=0 recording=0 paused=0' in out, out)
    check('online: frame advance is explained', 'online session: frame advance off' in err, err)
    check('online: nothing was rebased (no pause was held)', 'rebases=0' in out, out)

    # 8. a runtime pause command in an online session is refused
    stepb = os.path.join(WORK, 'b.cmd')
    hung, out, err = run_blocking(plain, ['online-pause-cmd'], {'DHD_TAS_STEP': stepb})
    check('online: a pause command does not block', (not hung) and 'FIELD_OK' in out, out + err)
    check('online: the pause command is refused', 'paused=0' in out, out + err)
    check('online: the refusal is named', "'pause' refused: online session" in err, err)

    # 9. a paused TAS entering an online session is released, not held
    stepc = os.path.join(WORK, 'c.cmd')
    hung, out, err = run_blocking(plain, ['paused-then-online'], {'DHD_TAS_PAUSE': '1', 'DHD_TAS_STEP': stepc})
    check('offline pause still holds (one step, then pause)', 'paused playing=0 recording=0 paused=1' in out, out)
    check('a paused TAS entering online is not blocked', (not hung) and 'FIELD_OK' in out, out + err)
    check('the pause is cleared on entering', 'after playing=0 recording=0 paused=0' in out, out)

    # 10. leaving the session leaves no stale state
    rc, out, err = run(plain, ['online-leave'], {'DHD_TAS_RECORD': os.path.join(WORK, 'r2.txt')})
    check('after leaving: fields run again', 'FIELDS 3' in out, out + err)
    check('after leaving: nothing paused', 'paused=0' in out, out)
    check('after leaving: the refusal stays latched', 'playing=0 recording=0' in out, out)

    # 11-15. strict movie validation, through the integration path
    cases = {
        'malformed (20 columns)': '1 4000 200 55 128 128' + PAD_TAIL[:-6] + '\n',
        'extra column': MOVIE + '3 0000 128 128 128 128' + PAD_TAIL + ' 7\n',
        'out of order': '2 4000 200 55 128 128' + PAD_TAIL + '\n1 0000 128 128 128 128' + PAD_TAIL + '\n',
        'duplicate field': '1 4000 200 55 128 128' + PAD_TAIL + '\n1 0000 128 128 128 128' + PAD_TAIL + '\n',
        'negative field': '-1 4000 200 55 128 128' + PAD_TAIL + '\n',
        'overflowing field': '18446744073709551616 4000 200 55 128 128' + PAD_TAIL + '\n',
        'stick out of range': '1 4000 200 256 128 128' + PAD_TAIL + '\n',
        'buttons more than 16 bits': '1 10000 200 55 128 128' + PAD_TAIL + '\n',
        'comment only': '# nothing here\n',
        'a truncated last line': MOVIE + '3 0000 128\n',
        'a truncated first line': '1 4000 200\n',
    }
    for name, text in cases.items():
        path = write('bad.txt', text)
        rc, out, err = run(plain, ['reject'], {'DHD_TAS_PLAY': path})
        ok = 'rejected playing=0' in out and 'pad_calls=0' in out and 'playback off' in err
        check('rejects ' + name, ok, out + err)

    # 16. allocation failure: nothing is played, not even the fields that loaded
    many = write('many.txt', ''.join('%d 4000 200 55 128 128%s\n' % (i + 1, PAD_TAIL) for i in range(50)))
    if fault['nomem']:
        rc, out, err = run(fault['nomem'], ['reject'], {'DHD_TAS_PLAY': many})
        check('allocation failure refuses the movie', 'rejected playing=0' in out, out + err)
        check('allocation failure plays nothing', 'pad_calls=0' in out and 'pad=4400 200 55' in out, out)
        check('allocation failure is explained', 'out of memory loading the movie' in err, err)
    else:
        for n in ('allocation failure refuses the movie', 'allocation failure plays nothing',
                  'allocation failure is explained'):
            check(n, None)

    # and the same movie plays on the normal binary, so the case above is the injection
    rc, out, err = run(plain, ['reject'], {'DHD_TAS_PLAY': many})
    check('the same movie plays without the injection', 'rejected playing=1' in out and 'pad_calls=1' in out, out)

    # 17. a missing movie is refused and does not block the game
    rc, out, err = run(plain, ['reject'], {'DHD_TAS_PLAY': os.path.join(WORK, 'no-such-movie.txt')})
    check('a missing movie is refused', 'rejected playing=0' in out and 'pad_calls=0' in out, out + err)
    check('a missing movie is explained', 'cannot read play movie' in err, err)

    # 18. without a pause the frame advance never holds the game
    hung, out, err = run_blocking(plain, ['nopause'], {})
    check('no pause: frames never block', (not hung) and 'after playing=0 recording=0 paused=0' in out,
          out + err)

    # 19. quit ends the game the way EXIT GAME does
    stepq = os.path.join(WORK, 'q.cmd')
    hung, out, err = run_blocking(plain, ['quit'], {'DHD_TAS_PAUSE': '1', 'DHD_TAS_STEP': stepq})
    check('quit: the window closes', 'window=1' in out, out + err)
    check('quit: state dropped', 'after quit: paused=0 playing=0 window=1' in out, out + err)
    check('quit: logged', 'quit: the game ends' in err, err)

    # 20. an unknown command is refused by name and does not block
    stepu = os.path.join(WORK, 'u.cmd')
    hung, out, err = run_blocking(plain, ['unknown'], {'DHD_TAS_STEP': stepu})
    check('unknown command: refused by name', "unknown command 'wat'" in err, err)
    check('unknown command: the field still runs', (not hung) and 'AFTER_UNKNOWN' in out, out + err)
    check('unknown command: the file is consumed', not os.path.exists(stepu))

    # 21. a write failure ends the recording, keeps the fields written before it, never blocks the game
    if os.name == 'nt':
        check('write failure: reported', None)          # no /dev/full on Windows
        check('write failure: the game still runs', None)
    else:
        rc, out, err = run(plain, ['rec3'], {'DHD_TAS_RECORD': '/dev/full'})
        check('write failure: reported',
              "recording stopped: cannot write the header on '/dev/full'" in err, err)
        check('write failure: the game still runs', 'f playing=0 recording=0' in out, out + err)
    if fault['flush']:
        flush = os.path.join(WORK, 'flush.txt')
        rc, out, err = run(fault['flush'], ['rec3'], {'DHD_TAS_RECORD': flush})
        check('a failed field write ends the recording', 'recording stopped: write failed on' in err, err)
        check('a failed field write counts only the fields before it',
              '1 field(s) complete before it' in err, err)
        check('a failed field write keeps the fields before it', 'f playing=0 recording=1' in out, out + err)
        kept = [l for l in open(flush).read().splitlines() if l and not l.startswith('#')]
        check('a failed field write keeps every complete line it wrote',
              bool(kept) and kept[0].startswith('1 '), str(kept))
    else:
        for n in ('a failed field write ends the recording',
                  'a failed field write counts only the fields before it',
                  'a failed field write keeps the fields before it',
                  'a failed field write keeps every complete line it wrote'):
            check(n, None)
    rc, out, err = run(plain, ['rec3'], {'DHD_TAS_RECORD': WORK})   # a directory: fopen fails
    check('unopenable record file: reported', 'cannot write' in err, err)
    check('unopenable record file: the game still runs', 'f playing=0 recording=0' in out, out + err)

    failed = [n for n, ok in results if ok is False]
    skipped = [n for n, ok in results if ok is None]
    tail = '%d checks, %d failed' % (len(results), len(failed))
    if skipped:
        tail += ', %d skipped' % len(skipped)
    say('\n' + tail)
    for n in failed:
        say('  failed: ' + n)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
