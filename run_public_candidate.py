#!/usr/bin/env python3
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,shutil,socket,subprocess,sys,time,urllib.request
import run_projection_migration as common
ROOT=Path(__file__).resolve().parent
BUILD=ROOT/'build/exchange-integration'
REPORT=ROOT/'public-candidate-verification'/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
BACKUP=Path.home()/'xgtx-mainnet-snapshot-20261015'
CANDIDATE=Path.home()/'xgtx-public-candidate-test-20261015'
EXPECTED_GENESIS='6cc86a50eca6febe795dc25490d02f29fe8a5590e93daf03d1e0c368191d7435'
EXPECTED_TIP='da3c85cdd98a97ea487ad52c58adc11eb0fce32d8ed3e9c5b842440bb7572567'
EXPECTED_SHA='69d876294a27e5c0346d61e6ee23e899e5b6184dcaab1a1458eb75d1f5801605'
common.ROOT=ROOT;common.REPORT=REPORT
run=common.run;check=common.check

def port():
    with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def rpc(address,method,params=None):
    data=json.dumps({'jsonrpc':'2.0','id':'candidate','method':method,'params':params or {}}).encode()
    request=urllib.request.Request(address+'/json_rpc',data=data,headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(request,timeout=2) as stream:out=json.load(stream)
    if 'error' in out:raise RuntimeError(str(out['error']))
    return out['result']
def info(address):
    with urllib.request.urlopen(address+'/get_info',timeout=2) as stream:return json.load(stream)
def stop(process):
    if process.poll() is None:
        process.terminate()
        try:process.wait(timeout=30)
        except subprocess.TimeoutExpired:process.kill();process.wait();raise RuntimeError('candidate daemon required forced termination')
def start_and_check(label):
    rpc_port,p2p_port=port(),port();address=f'http://127.0.0.1:{rpc_port}';log=REPORT/(label+'.log')
    args=[BUILD/'bin/xgtxd','--offline','--data-dir',CANDIDATE,'--p2p-bind-ip','127.0.0.1','--p2p-bind-port',str(p2p_port),'--rpc-bind-ip','127.0.0.1','--rpc-bind-port',str(rpc_port),'--rpc-ssl','disabled','--no-zmq','--no-igd','--non-interactive','--disable-dns-checkpoints','--check-updates','disabled','--log-level','1','--log-file',REPORT/(label+'.daemon.log')]
    print('Starting offline mainnet candidate copy; log: '+str(log),flush=True)
    with log.open('w') as output:
        process=subprocess.Popen([str(x) for x in args],cwd=ROOT,stdout=output,stderr=subprocess.STDOUT)
        try:
            limit=time.monotonic()+90
            while True:
                if process.poll() is not None:raise RuntimeError('candidate daemon exited; see '+str(log))
                try:state=info(address);break
                except Exception:
                    if time.monotonic()>limit:raise RuntimeError('candidate startup timed out; see '+str(log))
                    time.sleep(1)
            check(state['mainnet'] and state['offline'],'candidate uses mainnet identity in offline mode')
            check(state['height']==642 and state['top_block_hash']==EXPECTED_TIP,'candidate preserves native tip and height')
            check(state['incoming_connections_count']==0 and state['outgoing_connections_count']==0,'candidate has no network peers')
            ledger=rpc(address,'xgtx_get_exchange',{'limit':1})
            check(ledger['chain']==EXPECTED_GENESIS and ledger['tip']==EXPECTED_TIP and ledger['height']==642,'automatic startup migration aligns exchange projection')
            check(ledger['activation_height']==701 and ledger['native_backing'],'candidate reports fixed activation count and native backing')
            check(ledger.get('assets', [])==[] and ledger.get('pools', [])==[] and ledger['native_locked']=='0' and ledger['reserve_outputs']=='0','startup migration fabricates no economic credits')
            fork=rpc(address,'hard_fork_info',{'version':16})
            check(fork['version']==15 and not fork['enabled'] and fork['earliest_height']==700 and fork['voting']==16,'public fork is scheduled at700 and inactive at snapshot height')
            return {'info':state,'exchange':ledger,'fork':fork}
        finally:stop(process)

def main():
    REPORT.mkdir(parents=True)
    result={'status':'failed','launch_ready':False,'public_deployment':False,'activation_native_index':700,'activation_exchange_count':701,'report':str(REPORT)}
    try:
        check((BACKUP/'lmdb/data.mdb').is_file() and not (BACKUP/'lmdb/data.mdb').is_symlink(),'untouched backup exists')
        check(common.digest(BACKUP/'lmdb/data.mdb')==EXPECTED_SHA,'backup matches verified migration snapshot SHA256')
        check(not CANDIDATE.exists(),'new candidate copy path is unused')
        # No connection to server, wallet or remote service is made by this runner.
        run('01-policy-off',['g++','-std=c++17','-O1','-Wall','-Wextra','-Werror','tests/xgtx_public_activation/test_policy.cpp','-o',REPORT/'policy-off'])
        run('02-policy-off-tests',[REPORT/'policy-off'])
        run('03-policy-on',['g++','-std=c++17','-O1','-Wall','-Wextra','-Werror','-DXGTX_PUBLIC_EXCHANGE_CANDIDATE=1','tests/xgtx_public_activation/test_policy.cpp','-o',REPORT/'policy-on'])
        run('04-policy-on-tests',[REPORT/'policy-on'])
        run('05-configure',['cmake','-S','.', '-B',BUILD,'-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTS=ON','-DSTATIC=OFF','-DXGTX_PUBLIC_EXCHANGE_CANDIDATE=ON'])
        run('06-build',['cmake','--build',BUILD,'--target','daemon','xgtx_projection_migrate','xgtx_public_activation_tests','xgtx_projection_migration_tests','-j2'])
        run('07-native-boundary-tests',['ctest','--test-dir',BUILD,'-R','^xgtx_(public_activation|projection_migration)$','--no-tests=error','--output-on-failure','-V'])
        run('08-full-release-gate',['python3','run_release_verification.py'],timeout=7200)
        # Never use the phase34 migrated copy, whose temporary policy differs.
        shutil.copytree(BACKUP,CANDIDATE);os.chmod(CANDIDATE,0o700)
        before=common.inventory(run('09-copy-inventory-before',[BUILD/'bin/xgtx-projection-migrate','inspect',CANDIDATE/'lmdb']))
        check(before['height']==642 and before['genesis']==EXPECTED_GENESIS and before['tip']==EXPECTED_TIP,'fresh copy has verified chain identity')
        first=start_and_check('10-mainnet-startup');second=start_and_check('11-mainnet-restart')
        after=common.inventory(run('12-copy-inventory-after',[BUILD/'bin/xgtx-projection-migrate','inspect',CANDIDATE/'lmdb']))
        # Normal startup may update peers/properties. Native economic records
        # must be inspected via the full commitments rather than assuming parity.
        check(before==after,'all named native table commitments remain identical after startup/restart')
        check(first['exchange']==second['exchange'],'exchange state survives actual mainnet-candidate restart')
        check(common.digest(BACKUP/'lmdb/data.mdb')==EXPECTED_SHA,'untouched backup unchanged')
        result.update(status='passed',before=before,after=after,exchange=second['exchange'],fork=second['fork'])
        print('PASS: offline public activation candidate and restart. No live deployment; portable release validation still required.',flush=True)
    except Exception as error:result['error']=str(error);print('ERROR: '+str(error),file=sys.stderr,flush=True)
    finally:
        result['backup_unchanged']=(BACKUP/'lmdb/data.mdb').is_file() and common.digest(BACKUP/'lmdb/data.mdb')==EXPECTED_SHA
        if not result['backup_unchanged']:result['status']='failed'
        (REPORT/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('Result: '+str(REPORT/'result.json'),flush=True)
    return 0 if result['status']=='passed' else 1
if __name__=='__main__':sys.exit(main())
