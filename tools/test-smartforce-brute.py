"""Black-box tests. Builds nothing; all generated files go under --out-dir."""
import argparse
from collections import Counter
from pathlib import Path
import re
import subprocess
import uuid

ap = argparse.ArgumentParser()
ap.add_argument('--binary', required=True, type=Path)
ap.add_argument('--baseline', required=True, type=Path)
ap.add_argument('--out-dir', required=True, type=Path)
args = ap.parse_args()
args.out_dir = args.out_dir / uuid.uuid4().hex
args.out_dir.mkdir(parents=True, exist_ok=True)
serial = 0
route_re = re.compile(r'(?:[ndweaSqW][0-9]+)+')


def run(options, code=0, binary=None):
    global serial
    serial += 1
    p = subprocess.run([str(binary or args.binary), *options], cwd=args.out_dir,
                       text=True, capture_output=True, timeout=120)
    (args.out_dir / f'{serial:03}.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    assert p.returncode == code, (options, p.returncode, p.stdout, p.stderr)
    return p.stdout


def routes(text):
    return [s for line in text.splitlines() if route_re.fullmatch(s := line.strip())]


def listing(pattern, *extra, level=1):
    return routes(run(['smartforce', '--level', str(level), '--inputs', pattern, '--list', '--top', '0', *extra]))


def search(pattern, frame, *extra, level=1):
    name = args.out_dir / f'results-{serial + 1:03}.txt'
    run(['smartforce', '--level', str(level), '--inputs', pattern, '--report', f'alive@{frame}',
         '--top', '1', '--save', str(name), '--idle-up', '--update', '0', *extra])
    return routes(name.read_text())


def decode(route):
    return ''.join(c * int(n) for c, n in re.findall(r'([ndweaSqW])([0-9]+)', route))


def encode(frames):
    if not frames:
        return 'n0'
    return ''.join(f'{m.group()[0]}{len(m.group())}' for m in re.finditer(r'(.)\1*', frames))


pattern = 'n20e1<d30+b10..20,j5..6,30..40%>'
items = listing(pattern)
assert items
assert all(len(decode(s)) == 51 for s in items)
expected = [s for duration in range(10, 21)
            for s in listing(f'n20e1<d30+b{duration},j5..6,30..40%>')]
assert items == expected, 'inclusive duration range differs from separate searches'
assert Counter(listing(pattern, '--stochastic', '--seed', '123')) == Counter(items)
assert set(search(pattern, 51, '--threads', '1')) == set(items)
assert set(search(pattern, 51, '--threads', '12')) == set(items)
assert set(search(pattern, 51, '--threads', '12', '--stochastic', '--seed', '123')) == set(items)
print(f'PASS range, stochastic coverage, 1/12 threads: {len(items)} generated choices')

tail = listing('n20e1<e10d10w10+b10,j0>')
assert tail == ['n20e1d20w10'], 'Up inside the window was not replaced or the tail changed'
assert listing('n20e1<e3+b1,j0>') == ['n20e1d1e2']
assert listing('n20e1<d6+b6,j3,100..100%>') == ['n20e4d3']
assert listing('n20e1<d6+b6,j0,0%>') == ['n20e1d6']
assert listing('n20e1<d6+b6,j2,50..50%>') == []  # maximum 9: no integer at 50%
assert listing('n20e1<d5..8+b7..9,j1>') == listing('n20e1<d7+b7,j1>') + listing('n20e1<d8+b7..8,j1>')
print('PASS boundary durations, integer percentage bounds, short bases, and fixed tail')

fixture = args.out_dir / 'base routes.txt'
fixture.write_text('d30\na10d20\n')
file_items = listing(f'n20e1<{{{fixture}}}+b10..20,j5..6,30..40%>')
assert file_items == items + listing('n20e1<a10d20+b10..20,j5..6,30..40%>')
prefix_fixture = args.out_dir / 'prefix routes.txt'
prefix_fixture.write_text('n20e1\nn20e1d3\n')
file_prefix = listing(f'{{{prefix_fixture}}}<d15+b6..10,j1..3,25..75%>')
separate = listing('n20e1<d15+b6..10,j1..3,25..75%>') + listing('n20e1d3<d15+b6..10,j1..3,25..75%>')
assert file_prefix == separate, 'state-dependent choices leaked between prefixes'
print('PASS base files and different incoming states from prefix files')

multi = 'n20e1<d7+b4..6,j1..2,25..75%><d6+b3..5,j0..2,0..50%>d2'
multi_items = listing(multi)
assert multi_items
assert set(search(multi, 36, '--threads', '1')) == set(multi_items)
assert set(search(multi, 36, '--threads', '12')) == set(multi_items)
assert set(search(multi, 36, '--threads', '12', '--stochastic', '--seed', '987')) == set(multi_items)
print(f'PASS multiple dependent b blocks: {len(multi_items)} generated choices')

# A match before a b block must wait for a valid continuation, yet keep its
# original row and deciding-frame input prefix.
for extra in [[], ['--stochastic', '--seed', '321'], ['--hardfilter', '--tolerance', '0']]:
    assert search(multi, 1, '--threads', '12', *extra) == ['n1']
    run(['smartforce', '--level', '1', '--inputs', 'n20e1<d6+b6,j2,50..50%>',
         '--report', 'alive@1', '--threads', '12', '--update', '0', *extra], code=1)
    # Match during the first b block, followed by a second valid b block.
    early = search(multi, 24, '--threads', '12', *extra)
    assert set(early) == {encode(decode(s)[:24]) for s in multi_items}
print('PASS early matches wait for valid future b blocks and retain their deciding frames')

whole = decode('d4n1a1e1d10e2d5n2d1a2n1d1n3d10w1d2e1d35e3d21e19d19e1d14e1d54e1w9n1d9e3d2e1d1e33d1e1d2e8d9')
prefix = encode(whole[:282])
apex = prefix + '<d6+b6,j2>d1'
apex_items = listing(apex, level=17)
witness = encode(whole[:282] + 'ddeedd' + 'd')
assert witness in apex_items
assert set(search(apex, 289, '--threads', '12', level=17)) == set(apex_items)
assert set(search(apex, 289, '--threads', '12', '--stochastic', '--seed', '8', level=17)) == set(apex_items)
assert witness in listing(prefix + '<d6+b6,0..40%>d1', level=17)
for s in apex_items + items[::max(1, len(items)//12)]:
    level = 17 if len(decode(s)) == 289 else 1
    opts = ['run', '--level', str(level), '--inputs', s, '--hex']
    current = run(opts)
    accepted = run(opts, binary=args.baseline)
    assert current == accepted, 'native replay differs from accepted executable'
filtered = set(search(apex, 289, '--threads', '12', '--hardfilter', '--tolerance', '0', level=17))
assert filtered and filtered <= set(apex_items)
print('PASS near-apex witness, exact hardfilter, and accepted-port hex replay parity')

summary = run(['smartforce', '--level', '1', '--inputs', 'n20e1<d6+b6,j3,100..100%>', '--list'])
assert 'jump 1 - min: e3d3, max: e3d3' in summary
empty = run(['smartforce', '--level', '1', '--inputs', 'n20e1<d6+b6,j2,50..50%>', '--list'])
assert 'jump 1 - min: none, max: none' in empty
assert listing('n20e1<d6+b6,j0..3>', '--threads', '1', '--maxmem', '0.0015') == listing('n20e1<d6+b6,j0..3>')
run(['smartforce', '--level', '1', '--inputs', 'n20e1<d6+b6,j2,50..50%>', '--report', 'invalid'], code=2)
stopped = run(['smartforce', '--level', '1', '--inputs', 'n1..10000<d6+b6>',
               '--report', 'alive@1', '--stochastic', '--time', '0.001', '--threads', '12'], code=1)
assert 'stopped during jump preparation: time limit' in stopped
print('PASS prepared min/max, empty domain, memory fallback, invalid report, and preparation deadline')

for invalid in ['<d30+b>', '<d30+b0>', '<d30+b20..10>', '<d30:b10>', '<d5+b6>',
                '<d6+b6,j6>', '<d30+b10,j2,101%>', '<d30+b10,j2,40..30%>',
                '<d30+b10,j2,30..40>', '<d30+b10,j2,j3>', '<d30+b100001>',
                '(<d30+b10>|d30)', '<<d30+b10>+a1>']:
    run(['smartforce', '--level', '1', '--inputs', invalid, '--list'], code=2)
print('PASS invalid and unsupported nested forms rejected explicitly')
print('ALL BRUTE INTEGRATION TESTS PASSED')
print(f'Results: {args.out_dir}')
