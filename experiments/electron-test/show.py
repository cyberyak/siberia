import json,sys
r=json.loads([l for l in sys.stdin.read().splitlines() if l.startswith('{')][-1])
print({k:r.get(k) for k in ['cpuByProcess','cpuTotal','gpuUtilAvg']},'paint',r['paint'],'binding',r.get('binding'),'info',r.get('info'),'page',r['page'][-1:],'err',r['errors'][:2])
