import json,sys
r=json.loads(sys.stdin.read().strip().splitlines()[-1])
print({k:r.get(k) for k in ['cpuByProcess','cpuTotal','gpuUtilAvg']}, 'paintFps',r['paint'].get('fps'), 'blit',r['blit'], 'page',r['page'][-1:], 'err',r['errors'][:2],'streams',r.get('streams'),'noTex',r.get('noTexture'))
