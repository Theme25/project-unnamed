"""Resume must produce the exact suffix of the old seeded bijection; builds nothing."""
import argparse
from collections import Counter
from decimal import Decimal
from pathlib import Path
import re
import subprocess
import uuid

p=argparse.ArgumentParser()
p.add_argument('--binary',type=Path,required=True)
p.add_argument('--baseline',type=Path,required=True)
p.add_argument('--out-dir',type=Path,required=True)
a=p.parse_args(); binary=a.binary.resolve(); baseline=a.baseline.resolve()
folder=a.out_dir.resolve()/uuid.uuid4().hex; folder.mkdir(parents=True)
serial=0
def run(*args,exe=binary,code=0):
    global serial
    serial+=1
    r=subprocess.run([str(exe),'smartforce','--level','1',*args],cwd=folder,capture_output=True,text=True,timeout=60)
    (folder/f'{serial:03}.log').write_text(r.stdout+r.stderr)
    assert r.returncode==code,(args,r.returncode,r.stdout,r.stderr)
    return r.stdout
def routes(text):
    return [s for line in text.splitlines() if re.fullmatch(r'(?:[ndweaSqW]\d+)+',s:=line.strip())]
seed='553776661331543456'
pattern='(d1|n1|a1)'*5
baseargs=['--inputs',pattern,'--stochastic','--seed',seed]
old=routes(run(*baseargs,'--list','--top','0',exe=baseline))
assert len(old)==243 and len(set(old))==243
assert routes(run(*baseargs,'--list','--top','0'))==old
for percent in ['0','0%','.5','37.25','50','99.999999999','100%']:
    start=int(Decimal(percent.rstrip('%'))*len(old)/100)
    text=run(*baseargs,'--list','--top','0','--start-percent',percent)
    assert routes(text)==old[start:],percent
    assert f'start position: {start} of 243' in text
print('PASS exact old-bijection suffix, decimal rounding, zero and 100% boundaries')
start=90
for threads in ('1','12'):
    for samples in (0,7):
        target=folder/f'resumed-{threads}-{samples}.txt'
        text=run(*baseargs,'--start-percent','37.25','--samples',str(samples),'--threads',threads,
                 '--report','alive@5','--idle-up','--update','0','--top','1','--save',str(target))
        expected=old[start:][:samples or None]
        assert Counter(routes(target.read_text()))==Counter(expected),(threads,samples)
        assert f'position {start+len(expected)} of 243' in text
        assert f'this run: {len(expected)} positions visited after skipping 90' in text
        assert '# start-index 90' in target.read_text()
print('PASS one/twelve threads, new-sample limit, global progress, saved metadata, actual replay')
run(*baseargs,'--start-percent','100','--report','alive@5','--threads','12','--update','0',code=1)
saved=folder/'list.txt'
run(*baseargs,'--start-percent','50','--list','--top','1','--save',str(saved))
assert routes(saved.read_text())==old[121:]
# File-backed candidates must retain their existing order. This resembles attempt 3.
inputs=folder/'input strings.txt'; inputs.write_text('\n'.join(old)+'\n')
fileargs=['--inputs','{'+str(inputs)+'}','--stochastic','--seed',seed,'--list','--top','0']
full=routes(run(*fileargs,exe=baseline))
assert routes(run(*fileargs,'--start-percent','60'))==full[int(len(full)*.6):]
# Exclusions do not renumber the shuffle: the offset counts original candidate slots.
exclude=folder/'exclude.txt'; exclude.write_text('\n'.join(old[::4])+'\n')
assert routes(run(*baseargs,'--start-percent','50','--list','--top','0','--exclude',str(exclude)))==[q for q in old[121:] if q not in set(old[::4])]
print('PASS file-backed domain, listing save, exclusions preserve slot numbering')
# Prepared +b choices must also keep the old numbering.
brute=['--inputs','n20e1<d8+b6,j0..3>(d1|n1)','--stochastic','--seed',seed,'--list','--top','0']
full=routes(run(*brute,exe=baseline))
assert routes(run(*brute,'--start-percent','25'))==full[len(full)//4:]
print('PASS prepared jump-bruteforce domain retains numbering')
for bad in ['', '-1','100.000000001','101','nan','inf','2junk','1e1','1.1234567890','%','1%%','1..2']:
    run(*baseargs,'--start-percent',bad,'--list',code=2)
run(*baseargs,'--start-percent',code=2)
run('--inputs','d1','--start-percent','0',code=2)
run('--inputs','d1','--stochastic','--start-percent','5',code=2)
overflow='d0..100000a0..100000n0..100000w0..100000'
text=run('--inputs',overflow,'--stochastic','--seed','1','--start-percent','1','--list','--top','1',code=2)
assert 'exactly numbered search space' in (folder/f'{serial:03}.log').read_text()
print('PASS malformed percentages, missing seed, deterministic mode and unnumbered domain rejected')
helptext=run(code=2)
# Help goes to stderr and is saved in the corresponding numbered log.
assert '--start-percent P' in (folder/f'{serial:03}.log').read_text()
print('ALL RESUME TESTS PASSED',folder)
