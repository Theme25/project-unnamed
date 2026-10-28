"""Exact pre-replay exclusion and generator regressions; builds nothing."""
import argparse,re,subprocess,uuid
from pathlib import Path
from collections import Counter
p=argparse.ArgumentParser()
p.add_argument('--binary',required=True,type=Path)
p.add_argument('--out-dir',required=True,type=Path)
a=p.parse_args();binary=a.binary.resolve();folder=a.out_dir.resolve()/uuid.uuid4().hex
folder.mkdir(parents=True);serial=0
def run(*args,code=0):
    global serial
    serial+=1
    r=subprocess.run([str(binary),'smartforce','--level','1',*args],cwd=folder,capture_output=True,text=True,timeout=60)
    (folder/f'{serial:03}.log').write_text(r.stdout+r.stderr)
    assert r.returncode==code,(args,r.returncode,r.stdout,r.stderr)
    return r.stdout
def routes(text):return [s for line in text.splitlines() if re.fullmatch(r'(?:[ndweaSqW]\d+)+',s:=line.strip())]
pattern='n20e1<d8+b6,j0..3>(d1|n1)'
all_routes=routes(run('--inputs',pattern,'--list','--top','0'))
excluded=set(all_routes[::7]);path=folder/'saved strings.txt'
path.write_text('# saved results\nframe px py vx vy\n  30 0 0 0 0 0 1\n'+'\n'.join('  '+s for s in sorted(excluded))+'\n'+all_routes[0]+'\n')
expected=Counter(s for s in all_routes if s not in excluded)
for order in [[],['--stochastic','--seed','123']]:
    assert Counter(routes(run('--inputs',pattern,'--list','--top','0','--exclude',str(path),*order)))==expected
for threads in ['1','12']:
    saved=folder/f'search-{threads}.txt'
    log=run('--inputs',pattern,'--stochastic','--seed','123','--report','alive@30','--idle-up','--threads',threads,
            '--save',str(saved),'--exclude',str(path),'--top','1','--update','0')
    assert Counter(routes(saved.read_text()))==expected
    assert f'excluded {len(excluded)} exact strings before simulation' in log
print('PASS exact subtraction, same-prefix alternatives, prepared b, one/twelve threads, deterministic/random listing')
path.write_text('d1d2n0\nn0\n')
assert routes(run('--inputs','(d3|d3n1|d2|n0)','--list','--top','0','--exclude',str(path)))==['d2','d3n1']
path.write_text('d1\n')
log=run('--inputs','(d1|a1)','--stochastic','--report','alive@3','--pad','2','--exclude',str(path),'--top','0','--update','0')
assert 'excluded 1 exact strings before simulation' in log and 'a1n2' in log
path.write_text('n1\n')
log=run('--inputs','n2','--stochastic','--report','alive@1','--exclude',str(path),'--update','0')
assert 'excluded 0 exact strings before simulation' in log and '1 match' in log
path.write_text('n2\n')
log=run('--inputs','n2','--stochastic','--report','alive@1','--exclude',str(path),'--update','0',code=1)
assert 'excluded 1 exact strings before simulation' in log and '0 match' in log
print('PASS decoded RLE equality, empty string, whole strings rather than prefixes, pad, and early reports')
run('--inputs','d1','--stochastic','--exclude',str(folder/'missing.txt'),code=2)
path.write_text('# no routes\n')
run('--inputs','d1','--stochastic','--exclude',str(path),code=2)
path.write_text('n1000001\n')
run('--inputs','d1','--stochastic','--exclude',str(path),code=2)
path.write_text('d1\n')
run('--inputs','d1','--exclude',str(path),code=2)
run('--inputs','d1','--stochastic','--exclude',code=2)
print('PASS bad paths, empty files, oversized routes and unsupported invocation errors')
print('ALL EXCLUSION TESTS PASSED',folder)
