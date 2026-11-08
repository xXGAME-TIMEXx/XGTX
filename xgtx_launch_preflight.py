#!/usr/bin/env python3
"""Read-only source/build/public-metadata inventory. Never starts or edits a node."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import urllib.error
import urllib.parse
import urllib.request

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,*args,**kwargs):
        raise ValueError('RPC redirects are refused')

def rpc_url(value):
    parsed=urllib.parse.urlsplit(value)
    if parsed.scheme!='http' or parsed.hostname!='127.0.0.1' or parsed.username or parsed.password or parsed.path not in ('','/') or parsed.query or parsed.fragment:
        raise ValueError('Use http://127.0.0.1:PORT without credentials or a path')
    if not parsed.port or not 1<=parsed.port<=65535:raise ValueError('Invalid RPC port')
    return value.rstrip('/')+'/json_rpc'

def rpc(endpoint,method,params):
    if method not in ('get_info','get_block','hard_fork_info'):raise ValueError('Read-only method allowlist')
    payload=json.dumps({'jsonrpc':'2.0','id':'launch-preflight','method':method,'params':params}).encode()
    request=urllib.request.Request(rpc_url(endpoint),data=payload,headers={'Content-Type':'application/json'})
    # Ignore environment proxies; inspection stays on literal loopback.
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),NoRedirect())
    with opener.open(request,timeout=5) as response:
        data=response.read(256*1024+1)
    if len(data)>256*1024:raise ValueError('RPC response too large')
    result=json.loads(data)
    if result.get('error') or not isinstance(result.get('result'),dict):raise ValueError('RPC refused metadata request')
    result=result['result']
    if result.get('status','OK')!='OK':raise ValueError('RPC status failed')
    return result

def text(root,name):
    path=root/name
    if not path.is_file():return ''
    if path.stat().st_size>4*1024*1024:raise ValueError('Unexpected source size: '+name)
    return path.read_text(errors='strict')

def inventory(root,endpoint=None):
    root=Path(root).resolve()
    config=text(root,'src/cryptonote_config.h')
    forks=re.sub(r'/\*.*?\*/','',text(root,'src/hardforks/hardforks.cpp'),flags=re.S)
    core=text(root,'src/cryptonote_core/cryptonote_core.cpp')
    database=text(root,'src/blockchain_db/lmdb/db_lmdb.cpp')
    match=re.search(r'^#define\s+XGTX_INITIAL_PROTOCOL_VERSION\s+(\d+)',config,re.M)
    initial=int(match.group(1)) if match else None
    proof_match=re.search(r'^#define\s+HF_VERSION_VIEW_TAGS\s+(\d+)',config,re.M)
    proof_version=int(proof_match.group(1)) if proof_match else None
    tables={}
    for name in ('mainnet','testnet','stagenet'):
        match=re.search(r'const hardfork_t '+name+r'_hard_forks\[\]\s*=\s*\{(.*?)\n\};',forks,re.S)
        rows=[]
        if match:
            body=re.sub(r'//[^\n]*','',match.group(1))
            for version,height in re.findall(r'\{\s*(XGTX_INITIAL_PROTOCOL_VERSION|\d+)\s*,\s*(\d+)\s*,',body):
                rows.append({'version':initial if version=='XGTX_INITIAL_PROTOCOL_VERSION' else int(version),'block_index':int(height)})
        tables[name]=rows
    cache={}
    for line in text(root,'build/exchange-integration/CMakeCache.txt').splitlines():
        if line.startswith(('#','//')) or '=' not in line:continue
        k,v=line.split('=',1);key=k.split(':',1)[0]
        if key in ('ARCH','STATIC','CMAKE_BUILD_TYPE','BUILD_TAG') or key.startswith('XGTX_BUILD_'):cache[key]=v
    flags='\n'.join(text(root,p) for p in ('build/exchange-integration/src/daemon/CMakeFiles/daemon.dir/flags.make','build/exchange-integration/src/wallet/CMakeFiles/wallet_rpc_server.dir/flags.make'))
    cpu_flags=sorted(set(re.findall(r'-m(?:arch|tune)=[A-Za-z0-9_+.-]+',flags)))
    findings=[]
    def add(code,message):findings.append({'code':code,'message':message})
    if not tables['mainnet']:add('hardfork_schedule_unknown','Cannot establish the mainnet hard-fork schedule from source.')
    elif proof_version is None:add('proof_requirement_unknown','Cannot establish the tagged RingCT version from source.')
    elif not any(row['version'] is not None and row['version']>=proof_version for row in tables['mainnet']):
        add('modern_proof_schedule_missing','Mainnet does not schedule the tagged RingCT version required by the wallet bridge.')
    if 'native_db->configure_exchange_for_tests(config)' in core and 'if (exchange_regtest)' in core:
        add('public_activation_unimplemented','The inspected exchange startup configuration is guarded by experimental regtest mode. This is not evidence of a public activation implementation.')
    if 'Native/exchange startup state mismatch; use a fresh isolated database' in database:
        add('existing_chain_migration_unimplemented','Startup requires native and exchange heights to match. A reviewed migration/replay path for an existing chain is still needed.')
    if cache.get('ARCH') in (None,'','native') or any(f not in ('-march=x86-64','-mtune=generic') for f in cpu_flags):
        add('portable_release_unverified','The current integration build does not establish a portable public binary; use a separate release build and validate target CPU/dependencies.')
    files={}
    for name in ('src/cryptonote_config.h','src/hardforks/hardforks.cpp','src/cryptonote_core/cryptonote_core.cpp','src/blockchain_db/lmdb/db_lmdb.cpp','src/xgtx_exchange/engine.cpp','src/xgtx_exchange/lmdb_store.hpp'):
        source=text(root,name)
        if source:files[name]=hashlib.sha256(source.encode()).hexdigest()
    node={'status':'not_requested'}
    if endpoint:
        # Validate outside the connection-error handler, so unsafe URLs fail.
        rpc_url(endpoint)
        try:
            info=rpc(endpoint,'get_info',{})
            block=rpc(endpoint,'get_block',{'height':0})
            fork=rpc(endpoint,'hard_fork_info',{'version':proof_version}) if proof_version is not None else {}
            header=block.get('block_header',{})
            node={'status':'observed','info':{k:info[k] for k in ('height','top_block_hash','nettype','testnet','stagenet','offline','incoming_connections_count','outgoing_connections_count','version') if k in info},
                  'genesis':{k:header[k] for k in ('hash','height','major_version','minor_version') if k in header},
                  'modern_proofs':{k:fork[k] for k in ('version','enabled','earliest_height','voting','state') if k in fork}}
            if info.get('offline') or info.get('nettype') not in (None,'mainnet') or info.get('testnet') or info.get('stagenet'):
                add('observed_node_not_public_mainnet','Observed RPC node is offline or on a different network; do not use its height to schedule a public mainnet upgrade.')
        except (OSError,ValueError,KeyError,TypeError):
            node={'status':'unavailable','message':'No complete read-only node metadata obtained. No node was started and no credentials were read.'}
            add('live_chain_metadata_missing','Need the existing mainnet genesis, current height and fork metadata before selecting an activation plan.')
    else:add('live_chain_metadata_missing','No existing mainnet node was inspected; activation height remains undecided.')
    return {'schema':1,'scope':'read-only activation inventory; not a security audit or launch approval','created_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'launch_ready':False,'source_sha256':files,'hardfork_tables':tables,'view_tags_protocol_version':proof_version,'build':{'cache':cache,'observed_cpu_flags':cpu_flags},'node':node,'blockers':findings}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path.home()/'xgtx-exchange-dev')
    parser.add_argument('--rpc-url',default='http://127.0.0.1:48081')
    args=parser.parse_args();root=args.root.resolve()
    if not (root/'src/cryptonote_config.h').is_file():parser.error('Expected XGTX source root')
    report=inventory(root,args.rpc_url)
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output=root/'launch-preflight'/stamp;output.mkdir(parents=True,exist_ok=False)
    path=output/'result.json';path.write_text(json.dumps(report,indent=2)+'\n')
    print('PUBLIC LAUNCH: NOT READY')
    for item in report['blockers']:print(item['code']+': '+item['message'])
    print('Node metadata: '+report['node']['status'])
    print('Read-only report: '+str(path))
if __name__=='__main__':main()
