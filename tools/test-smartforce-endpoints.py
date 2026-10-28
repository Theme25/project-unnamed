"""Regression tests for successful endpoint deduplication. Builds nothing."""
import argparse,json,re,subprocess,uuid
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--binary',type=Path,required=True)
p.add_argument('--baseline',type=Path,help='Optional executable from before endpoint deduplication')
p.add_argument('--pair',type=Path,help='Optional two equivalent routes for a custom regression')
p.add_argument('--three',type=Path,help='The same two routes plus one distinct endpoint')
p.add_argument('--frame',type=int,default=45,help='Custom fixture endpoint frame')
p.add_argument('--out-dir',type=Path,required=True)
a=p.parse_args(); folder=a.out_dir.resolve()/uuid.uuid4().hex;folder.mkdir(parents=True)
serial=0
def run(pattern,frame,*extra,exe=None,level=17):
    global serial
    serial+=1; target=folder/f'{serial:03}.txt'
    args=[str(exe or a.binary),'smartforce','--level',str(level),'--inputs',pattern,'--report',f'alive@{frame}',
          '--idle-up','--top','0','--update','0','--save',str(target),*extra]
    r=subprocess.run(args,cwd=folder,capture_output=True,text=True,timeout=120)
    (folder/f'{serial:03}.log').write_text(r.stdout+r.stderr)
    assert r.returncode==0,(args,r.stdout,r.stderr)
    text=target.read_text()
    routes=[s for l in text.splitlines() if re.fullmatch(r'(?:[ndweaSqW]\d+)+',s:=l.strip())]
    counts=[int(l.split()[-1]) for l in text.splitlines() if re.match(r'^\s+\d+\s+-?\d+\.\d+',l)]
    assert len(routes)==len(counts)
    return routes,sum(counts)
if bool(a.pair) != bool(a.three): p.error('--pair and --three must be supplied together')
if a.pair:
    pair_frame=a.frame
else:
    # Up on the first airborne frame has no effect; the next n frame also clears
    # its diagnostic flag. These small fixtures need no private search outputs.
    a.pair=folder/'pair.txt';a.three=folder/'three.txt';pair_frame=2
    a.pair.write_text('n2\nw1n1\n');a.three.write_text('n2\nw1n1\nd2\n')
pair='{'+str(a.pair.resolve())+'}'; three='{'+str(a.three.resolve())+'}'
hard=['--hardfilter','--tolerance','0','--raw-states']
if a.baseline:
    assert len(run(pair,pair_frame,*hard,exe=a.baseline)[0])==2,'Baseline must reproduce duplicate endpoints'
for threads in ['1','12']:
    for random in [[],['--stochastic','--seed','1833432511663435572']]:
        for every in ['0','32']:
            args=[*hard,'--threads',threads,'--merge-every',every,*random]
            assert run(pair,pair_frame,*args)[1]==1
            rows,count=run(three,pair_frame,*args);assert len(rows)==count==2
            assert len(run(three,pair_frame,'--threads',threads,*random)[0])==3
print('PASS Level 17 endpoint convergence; distinct states retained; hardfilter off unchanged; 1/12 threads and both orders')
for frame in [31,32,33]:
    pattern=f'(n{frame}|n{frame+1})'
    assert run(pattern,frame,'--threads','1',exe=a.baseline)[1]==2
    for random in [[],['--stochastic','--seed','3']]:
        assert run(pattern,frame,*hard,'--threads','12',*random)[1]==1
assert run('(d1|d1)',5,*hard,'--pad','4','--threads','12')[1]==1
print('PASS successful endpoints before/on/after interval boundaries, early matches, and padding')
brute='n20e1<d8+b6,j0..3>'
assert run(brute,1,level=1,exe=a.baseline)[1]>1
for random in [[],['--stochastic','--seed','17']]:
    assert run(brute,1,*hard,'--threads','12',*random,level=1)[1]==1
print('PASS deferred matches before state-dependent jump blocks use original saved endpoint')
for random in [[],['--stochastic','--seed','22']]:
    assert run(pair,pair_frame,'--hardfilter','--threads','12',*random)[1]==1
    assert run(pair,pair_frame,'--merge-report','--tolerance','0','--raw-states',*random)[1]==1
print('PASS default canonical comparison and merge diagnostics')
print('ALL ENDPOINT TESTS PASSED',folder)
