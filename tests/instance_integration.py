"""Read-only instance, structure and pointer navigation checks on a probed UE device."""
import argparse
import json
from pathlib import Path
import time
import urllib.error
import urllib.request

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base',default='http://127.0.0.1:18766')
    parser.add_argument('--address')
    parser.add_argument('--out',type=Path,default=Path('artifacts/instance-integration.json'))
    args=parser.parse_args()
    def api(request):
        req=urllib.request.Request(args.base+'/api/explorer',data=json.dumps(request).encode(),
                                  headers={'Content-Type':'application/json','X-UMT-Client':'1'})
        try:
            with urllib.request.urlopen(req,timeout=20) as response:return response.status,json.load(response)
        except urllib.error.HTTPError as e:return e.code,json.load(e)
    address=args.address
    if not address:
        cursor=0
        for _ in range(250):
            code,page=api({'action':'list','query':'Default__SceneComponent','cursor':cursor,'limit':8,'scanLimit':4096})
            assert code==200,page
            match=next((o for o in page['items'] if o['name']=='Default__SceneComponent'),None)
            if match:address=match['address'];break
            if page['complete']:break
            cursor=page['nextCursor']
    assert address,'No SceneComponent instance found; pass --address for one'
    code,obj=api({'action':'inspect','kind':'instance','address':address})
    assert code==200,obj
    assert obj['isInstance'] and obj['identity'] and obj['classHierarchy']
    assert obj['properties'] and obj['functions'] and any(f.get('inherited') for f in obj['functions'])
    assert all('owner' in p and 'offset' in p and 'address' in p for p in obj['properties'])
    vectors=[p for p in obj['properties'] if p.get('valueKind')=='vector3']
    assert vectors,'No supported FVector fields on selected instance'
    vec=vectors[0]
    code,detail=api({'action':'inspect','kind':'struct','address':vec['address'],'typeAddress':vec['structAddress']})
    assert code==200,detail
    for axis in ('X','Y','Z'):
        row=next(p for p in detail['properties'] if p['name']==axis)
        assert abs(float(row['value'])-vec['components'][axis])<=max(1,abs(vec['components'][axis]))*1e-7
        assert int(row['offset'],16)==('XYZ'.index(axis)*(4 if vec['scalarType']=='float32' else 8))
    class_slot=next(m['value'] for m in obj['metadata'] if m['name']=='ClassPrivate').split()[0]
    # This project exposes the recovered UObject ClassPrivate offset in /api/state.
    with urllib.request.urlopen(args.base+'/api/state',timeout=10) as response:state=json.load(response)
    group=next(g for g in state['groups'] if g['name']=='UObject')
    offset=int(next(f['offset'] for f in group['fields'] if f['name']=='ClassPrivate'),16)
    code,cls=api({'action':'inspect','kind':'object','address':hex(int(address,16)+offset),'dereference':True})
    assert code==200 and int(cls['address'],16)==int(class_slot,16),cls
    for invalid in ({'kind':'instance','address':obj['classAddress']},
                    {'kind':'struct','address':vec['address']},
                    {'kind':'struct','address':vec['address'],'typeAddress':obj['classAddress']},
                    {'kind':'instance','address':address,'expectedIdentity':'stale'},
                    {'kind':'instance','address':address,'generation':obj['generation']+100}):
        status,error=api({'action':'inspect',**invalid})
        assert status>=400 and error.get('error'),error
    time.sleep(.05)
    code,again=api({'action':'inspect','kind':'instance','address':address,'generation':obj['generation'],'expectedIdentity':obj['identity']})
    assert code==200 and again['sampledAtMs']>obj['sampledAtMs'],again
    report={'passed':True,'instance':obj,'vectorStructure':detail,'classPointerTarget':cls['address'],
            'checks':['instance members/inheritance/functions','vector floats match typed struct fields',
                      'pointer-slot dereference','class-vs-instance rejection','invalid type rejection',
                      'generation/identity guards','fresh repeat sample']}
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(json.dumps(report,ensure_ascii=False,indent=2),'utf-8')
    print('PASS:',len(report['checks']),'checks;',len(obj['properties']),'members;',len(obj['functions']),'functions')

if __name__=='__main__':main()
