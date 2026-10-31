"""Read-only live World/ULevel.Actors integration. Target must already be probed."""
import argparse
import json
from pathlib import Path
import time
import urllib.error
import urllib.request

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base',default='http://127.0.0.1:18766')
    parser.add_argument('--out',type=Path,default=Path('artifacts/world-integration.json'))
    args=parser.parse_args()
    def api(data):
        req=urllib.request.Request(args.base+'/api/explorer',data=json.dumps(data).encode(),headers={'Content-Type':'application/json','X-UMT-Client':'1'})
        for attempt in range(30):
            try:
                with urllib.request.urlopen(req,timeout=20) as r:return r.status,json.load(r)
            except urllib.error.HTTPError as e:
                error=json.load(e)
                if error.get('busy') and attempt<29:time.sleep(.15);continue
                return e.code,error
    start=time.monotonic();cursor=0;worlds=[]
    while True:
        code,page=api({'action':'worlds','cursor':cursor,'limit':64,'scanLimit':8192})
        assert code==200,page
        worlds+=page['items']
        if page['complete']:break
        assert page['nextCursor']>cursor
        cursor=page['nextCursor']
    discovery=time.monotonic()-start
    assert worlds,'No live World; enter a scene'
    world=sorted(worlds,key=lambda w:(w['hasGameInstance'],w['levelCount']),reverse=True)[0]
    start=time.monotonic();cursor=0;actors=[];topology=None;invalid=0
    while True:
        code,page=api({'action':'actors','worldAddress':world['address'],'cursor':cursor,'limit':500,'scanLimit':8192,
                       **({'expectedTopology':topology} if topology else {})})
        assert code==200,page
        topology=page['topology'];actors+=page['items'];invalid+=page['invalidSlots']
        if page['complete']:break
        assert page['nextCursor']>cursor
        cursor=page['nextCursor']
    elapsed=time.monotonic()-start
    assert actors,'World has no currently readable Actors'
    assert len({a['address'] for a in actors})==len(actors),'Duplicate Actor pointers across pages'
    assert len(actors)+invalid==page['totalSlots']
    levels={l['address']:l for l in page['levels']}
    assert all(a['levelAddress'] in levels and a['actorSlot']<levels[a['levelAddress']]['count'] for a in actors)
    samples=[]
    for actor in (actors[0],actors[len(actors)//2],actors[-1]):
        code,target=api({'action':'inspect','kind':'instance','address':actor['slotAddress'],'dereference':True})
        assert code==200,target
        assert target['address']==actor['address'] and target['name']==actor['name']
        assert target['properties'] and target['functions']
        samples.append({'actor':actor,'memberCount':len(target['properties']),'functionCount':len(target['functions']),
                        'vectorCount':sum(p.get('valueKind')=='vector3' for p in target['properties'])})
    chosen=next(l for l in page['levels'] if l['count']>0)
    code,filtered=api({'action':'actors','worldAddress':world['address'],'levelAddress':chosen['address'].lower(),'limit':1000})
    assert code==200 and filtered['totalSlots']==chosen['count'],filtered
    assert all(a['levelAddress']==chosen['address'] for a in filtered['items'])
    code,error=api({'action':'actors','worldAddress':world['address'],'expectedTopology':'outdated'})
    assert code>=400 and error.get('changed'),error
    code,error=api({'action':'actors','worldAddress':world['address'],'generation':page['generation']+1})
    assert code>=400 and 'generation' in error,error
    code,error=api({'action':'actors','worldAddress':actors[0]['address']})
    assert code>=400 and error.get('error'),error
    report={'passed':True,'world':world,'worldCount':len(worlds),'levels':page['levels'],'actorsOffset':page['actorsOffset'],
            'totalSlots':page['totalSlots'],'actorCount':len(actors),'invalidSlots':invalid,'discoverySeconds':round(discovery,4),
            'traversalSeconds':round(elapsed,4),'samples':samples,'warnings':page['warnings'],'topology':topology,
            'generation':page['generation']}
    args.out.parent.mkdir(parents=True,exist_ok=True);args.out.write_text(json.dumps(report,ensure_ascii=False,indent=2),'utf-8')
    print(json.dumps({k:report[k] for k in ('passed','worldCount','totalSlots','actorCount','invalidSlots','discoverySeconds','traversalSeconds')},ensure_ascii=False))

if __name__=='__main__':main()
