"""Independent reduction coverage and CLI regression tests. Builds nothing."""
import argparse
from collections import Counter
from itertools import combinations, groupby
from math import comb
from pathlib import Path
import re
import subprocess
import uuid

ap = argparse.ArgumentParser()
ap.add_argument('--binary', required=True, type=Path)
ap.add_argument('--baseline', required=True, type=Path)
ap.add_argument('--out-dir', required=True, type=Path)
args = ap.parse_args()
args.out_dir = args.out_dir.resolve() / uuid.uuid4().hex
args.out_dir.mkdir(parents=True)
args.binary = args.binary.resolve()
args.baseline = args.baseline.resolve()
serial = 0
letters = 'ndweaSqW'
route_re = re.compile(r'(?:[ndweaSqW][0-9]+)+')


def run(options, code=0, binary=None):
    global serial
    serial += 1
    result = subprocess.run([str(binary or args.binary), *options], cwd=args.out_dir,
                            text=True, capture_output=True, timeout=120)
    text = result.stdout + result.stderr
    (args.out_dir / f'{serial:03}.log').write_text(text, encoding='utf-8')
    assert result.returncode == code, (options, result.returncode, text)
    return text


def routes(text):
    return [s for line in text.splitlines() if route_re.fullmatch(s := line.strip())]


def listing(pattern, *extra, binary=None):
    return routes(run(['smartforce', '--level', '17', '--inputs', pattern,
                       '--list', '--top', '0', *extra], binary=binary))


def decode(route):
    return ''.join(c * int(n) for c, n in re.findall(r'([ndweaSqW])([0-9]+)', route))


def encode(frames):
    return ''.join(f'{c}{len(list(g))}' for c, g in groupby(frames)) or 'n0'


def unique(items):
    return list(dict.fromkeys(items))


def signature(frames, pressed):
    indices = [len(frames) - i for i, c in enumerate(frames) if c == pressed]
    return len(indices), sum(indices)


def check_sums(length, low, high, minimum=0, maximum=100, op='+'):
    # Exhaust all subsets; do not reuse the generator's formula or placement rule.
    full = {(k, sum(positions)) for k in range(low, min(high, length) + 1)
            for positions in combinations(range(1, length + 1), k)}
    largest = max(s for _, s in full)
    floor, cap = (minimum * largest + 99) // 100, maximum * largest // 100
    expected = sorted((pair for pair in full if floor <= pair[1] <= cap),
                      key=lambda pair: (pair[0], -pair[1]))
    pattern = f'<d{length}{op}a{low}..{high}, r, {minimum}..{maximum}%>'
    if not expected:
        run(['smartforce', '--level', '17', '--inputs', pattern, '--list'], code=2)
        return
    actual = listing(pattern)
    pressed = 'S' if op == '+' else 'a'
    for route in actual:
        frames = decode(route)
        assert len(frames) == length and set(frames) <= {'d', pressed}, (pattern, route)
    assert [signature(decode(s), pressed) for s in actual] == expected, pattern


for op in ['+', ':']:
    for length in range(13):
        check_sums(length, 0, length, op=op)
    for bounds in [(0, 0), (1, 1), (25, 50), (50, 50), (99, 100), (100, 100)]:
        check_sums(10, 0, 6, *bounds, op=op)
    check_sums(4, 1, 8, 25, 75, op)
    check_sums(10, 1, 2, 50, 50, op)  # no integer sum at exactly 50% of 19
print('PASS exhaustive subset signatures, order, zero/full counts, and inclusive percentage bounds')

# The accepted j generator chooses the representatives; compare that order
# independently of the new letter handling, including frames already holding keys.
mask_routes = listing('<n8+j0..4>', binary=args.baseline)
base = 'ndweaSqW'
for op in ['+', ':']:
    for key in letters:
        expected = []
        for mask_route in mask_routes:
            output = list(base)
            for i, c in enumerate(decode(mask_route)):
                if c == 'w':
                    output[i] = letters[letters.index(base[i]) | letters.index(key)] if op == '+' else key
            expected.append(encode(output))
        assert listing(f'<{encode(base)}{op}{key}0..4,r>') == unique(expected), (op, key)
assert listing('<a8+a0..4,r>') == ['a8']
assert listing('<d0+a0,r>') == ['n0']
assert listing('<d8+a8,r>') == ['S8']
assert listing('<d8:a8,r>') == ['a8']
assert listing('<d8+a,r>') == listing('<d8+a1,r>')
assert listing('<d8+a0..4,r>') == listing('<d8+a0..4,r,0..100%>')
assert listing('<d8+a0..4,r,50%>') == listing('<d8+a0..4,r,0..50%>')
assert listing('<d8+w0..4,r>') == listing('<d8+j0..4>', binary=args.baseline)
print('PASS all input letters, add/overwrite semantics, no-op deduplication, and legacy representative order')

pattern = '<d30+a1..5,r>'
reduced = listing(pattern)
assert len(reduced) == 400 and sum(comb(30, k) for k in range(1, 6)) == 174436
lazy_pattern = '<d30+a0..30,r,25..75%>'
stored = listing(lazy_pattern, '--maxmem', '0.1')
lazy = listing(lazy_pattern, '--maxmem', '0.0001')
assert lazy == stored
assert Counter(listing(lazy_pattern, '--maxmem', '0.0001', '--stochastic', '--seed', '781')) == Counter(stored)
assert listing('<d3..9+a0..3,r,25..75%>') == [route for length in range(3, 10)
        for route in listing(f'<d{length}+a0..3,r,25..75%>')]
assert listing('(<d3..9+a0..3,r,25..75%>)=6..7') == listing('<d6..7+a0..3,r,25..75%>')
fixture = args.out_dir / 'base routes with spaces.txt'
fixture.write_text('d4n2\nn6\nd8\n', encoding='utf-8')
assert listing(f'<{{{fixture}}}[-1]+a1..2,r,25..70%>') == unique([
    route for base_route in ['d4n1', 'n5', 'd7']
    for route in listing(f'<{base_route}+a1..2,r,25..70%>')])
inner = listing('<n6+a1..2,r>')
outer_masks = [decode(s) for s in listing('<n6+j1..2>', binary=args.baseline)]
expected = unique(encode(''.join(letters[letters.index(c) | (1 if mask[i] == 'w' else 0)]
                                for i, c in enumerate(decode(s))))
                  for s in inner for mask in outer_masks)
assert listing('<<n6+a1..2,r>+d1..2,r>') == expected
# An ordinary placement can wrap r too; both nodes share a scratch buffer.
assert listing('<<n6+a1..2,r>+d1>') == unique(route for s in inner for route in listing(f'<{s}+d1>'))
assert listing('(<d4:a1,r>|<n5+d2,r>)') == listing('<d4:a1,r>') + listing('<n5+d2,r>')
print('PASS 400 versus 174436 placements, stored/lazy parity, stochastic coverage, files, lengths, and nesting')

# Listing r must be independent of level and incoming velocity/state.
for level, prefix in [(1, 'n20'), (17, 'e1d10'), (17, 'n50')]:
    whole = routes(run(['smartforce', '--level', str(level), '--inputs', prefix + pattern, '--list', '--top', '0']))
    assert [decode(s)[len(decode(prefix)):] for s in whole] == [decode(s) for s in reduced]


def saved_search(search_pattern, frame, *extra):
    name = args.out_dir / f'saved-{serial + 1:03}.txt'
    run(['smartforce', '--level', '17', '--inputs', search_pattern, '--report', f'alive@{frame}',
         '--top', '1', '--save', str(name), '--idle-up', '--update', '0', *extra])
    return routes(name.read_text())


assert set(saved_search(pattern, 30, '--threads', '1')) == set(reduced)
assert set(saved_search(pattern, 30, '--threads', '12')) == set(reduced)
assert set(saved_search(pattern, 30, '--threads', '12', '--stochastic', '--seed', '17')) == set(reduced)
filtered = set(saved_search(pattern, 30, '--threads', '12', '--hardfilter', '--tolerance', '0'))
assert filtered and filtered <= set(reduced)
# r inside b's BASE and before b remains static; b still uses the incoming Sim.
brute_pattern = 'n20e1<<d8+a1..2,r>+b6,j1..2>'
brute_routes = listing(brute_pattern)
assert brute_routes == [route for base_route in listing('<d8+a1..2,r>')
                        for route in listing('n20e1<' + base_route + '+b6,j1..2>')]
assert set(saved_search(brute_pattern, 29, '--threads', '12')) == set(brute_routes)
print('PASS game-independent listing, 1/12 threads, exact hardfilter, stochastic search, uncapped save, and b integration')

for pattern in ['<d8+a0..4>', '<d8:a1..3,w0..1>', '<n8+j>', '<d8:j1..4,25..75%>',
                '<<n6+a1>+w2>', 'n20e1<d8+b6,j1..2>']:
    assert listing(pattern) == listing(pattern, binary=args.baseline), pattern
assert 'jump 1 -' not in run(['smartforce', '--level', '17', '--inputs', '<d8+a1..4,r>', '--list'])
summary = run(['smartforce', '--level', '17', '--inputs', '<d4+a1,r><d4+j1>', '--list'])
assert 'jump 1 - min: d3e1, max: e1d3' in summary and 'jump 2 -' not in summary
for route in reduced[::80] + [brute_routes[0]]:
    options = ['run', '--level', '17', '--inputs', route, '--hex']
    assert run(options) == run(options, binary=args.baseline), route
print('PASS old syntax and jump numbering unchanged, accepted-port per-frame hexadecimal replay parity')

for pattern in ['<d8+r>', '<d8+a1,r1>', '<d8+a1,r,r>', '<d8+a1,d1,r>',
                '<d8+a1,r,d1>', '<d8+a1,r,101%>', '<d8+a1,r,-1%>',
                '<d8+a1,r,50..25%>', '<d8+a1,r,25.5..50%>', '<d8+a1,r,25..%>',
                '<d8+a1,r,25..50>', '<d8+a1,r,>', '<d8+a5..1,r>', '<d8+a9,r>',
                '<d0+a1,r>', '<d8+a1,50%,r>', '<d8+j1,r>', '<d8+b6,j1,r>']:
    run(['smartforce', '--level', '17', '--inputs', pattern, '--list'], code=2)
print('PASS malformed and ambiguous reductions rejected explicitly')
print('ALL REDUCED TAP TESTS PASSED')
print(f'Results: {args.out_dir}')
