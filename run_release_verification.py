#!/usr/bin/env python3
"""Complete local release-candidate regression gate. Does not enable a public network."""
import datetime
import json
import os
from pathlib import Path
import subprocess
import signal
import sys

SUITES={
 'xgtx_exchange_native':'XGTX_BUILD_EXCHANGE_NATIVE_TESTS',
 'xgtx_fee_recipient':'XGTX_BUILD_FEE_RECIPIENT_TESTS',
 'xgtx_native_carrier':'XGTX_BUILD_NATIVE_CARRIER_TESTS',
 'xgtx_native_db':'XGTX_BUILD_NATIVE_DB_TESTS',
 'xgtx_native_mempool':'XGTX_BUILD_NATIVE_MEMPOOL_TESTS',
 'xgtx_signed_carrier':'XGTX_BUILD_SIGNED_CARRIER_TESTS',
 'xgtx_reserve_proofs':'XGTX_BUILD_RESERVE_PROOFS_TESTS',
 'xgtx_reserve_storage':'XGTX_BUILD_RESERVE_STORAGE_TESTS',
 'xgtx_backed_ledger':'XGTX_BUILD_BACKED_LEDGER_TESTS',
 'xgtx_native_deposit':'XGTX_BUILD_NATIVE_DEPOSIT_TESTS',
 'xgtx_native_withdraw':'XGTX_BUILD_NATIVE_WITHDRAW_TESTS',
 'xgtx_state_undo':'XGTX_BUILD_STATE_UNDO_TESTS',
 'xgtx_bootstrap_budget':'XGTX_BUILD_BOOTSTRAP_BUDGET_TESTS',
}
LIFECYCLES=[('node-smoke','test_node_smoke.py'),('funded-carrier','test_funded_carrier.py'),
 ('exchange-lifecycle','test_exchange_lifecycle.py'),('wallet-bootstrap-interface','test_wallet_bootstrap.py'),
 ('two-node-reorg','test_loopback_network.py')]

def validate_inventory(data,build):
    tests=data.get('tests')
    if not isinstance(tests,list):raise ValueError('CTest inventory missing tests')
    rows={}
    for test in tests:
        name=test.get('name')
        if name in rows:raise ValueError('Duplicate CTest name: '+str(name))
        rows[name]=test
    missing=set(SUITES)-rows.keys()
    if missing:raise ValueError('Missing required suites: '+', '.join(sorted(missing)))
    selected=sorted(name for name in rows if isinstance(name,str) and name.startswith('xgtx_'))
    root=Path(build).resolve()
    for name in selected:
        command=rows[name].get('command',[])
        if not command or not isinstance(command[0],str):raise ValueError('Missing test executable: '+name)
        executable=Path(command[0])
        if not executable.is_absolute():raise ValueError('Unresolved test executable: '+name)
        executable=executable.resolve()
        if not executable.is_relative_to(root) or not executable.is_file() or not os.access(executable,os.X_OK):raise ValueError('Unbuilt or unexpected test executable: '+name)
    return selected

def main():
    root=Path.home()/'xgtx-exchange-dev';build=root/'build/exchange-integration'
    for _,script in LIFECYCLES:
        if not (root/script).is_file():raise RuntimeError('Missing Phase32 prerequisite: '+script)
    if not (root/'tests/xgtx_backed_ledger/CMakeLists.txt').is_file():raise RuntimeError('Missing backed ledger source')
    if not (root/'xgtx-trader/test_server.py').is_file():raise RuntimeError('Missing interface HTTP tests')
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output=root/'release-verification'/stamp;output.mkdir(parents=True,exist_ok=False)
    report={'schema':1,'scope':'complete local candidate regression; no public activation','launch_ready':False,'status':'running','required_suites':sorted(SUITES),'steps':[]}
    env=dict(os.environ);env['DNS_PUBLIC']='tcp://9.9.9.9';env.pop('XGTX_TRADER_DEMO',None)
    def save():(output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    def run(name,command):
        log=output/(str(len(report['steps'])+1).zfill(2)+'-'+name+'.log')
        step={'name':name,'status':'running','log':str(log)};report['steps'].append(step);save()
        print('Running '+name+'; progress log: '+str(log),flush=True)
        with log.open('w') as stream:
            process=subprocess.Popen(command,cwd=root,env=env,stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
            try:
                while True:
                    try:code=process.wait(timeout=15);break
                    except subprocess.TimeoutExpired:
                        with log.open('rb') as tail:
                            tail.seek(0,2);tail.seek(max(0,tail.tell()-1024));lines=tail.read().decode(errors='replace').splitlines()
                        print(name+' still running: '+(lines[-1][-180:] if lines else 'waiting for output'),flush=True)
            except BaseException:
                # Stop only this runner's new process group, including its
                # temporary fixture children. Never search/kill existing nodes.
                for sig,timeout in ((signal.SIGINT,10),(signal.SIGTERM,5),(signal.SIGKILL,5)):
                    try:os.killpg(process.pid,sig)
                    except ProcessLookupError:break
                    try:process.wait(timeout=timeout);break
                    except subprocess.TimeoutExpired:continue
                raise
        step['exit_code']=code;step['status']='passed' if code==0 else 'failed';save()
        if code:
            print('\n'.join(log.read_text(errors='replace').splitlines()[-70:]),flush=True)
            raise RuntimeError(name+' failed; see '+str(log))
        print('PASS: '+name,flush=True)
    save();print('Report directory: '+str(output),flush=True)
    try:
        configure=['cmake','-S',str(root),'-B',str(build),'-DBUILD_TESTS=ON']
        configure+=['-D'+flag+'=ON' for flag in SUITES.values()]
        configure+=['-DXGTX_BUILD_NODE_SMOKE=ON','-DXGTX_BUILD_FUNDED_CARRIER_TESTS=ON','-DXGTX_BUILD_BACKED_DAEMON_FIXTURE=ON']
        run('configure',configure)
        targets=['daemon','wallet_rpc_server','unit_tests','test_notifier','xgtx_projection_inspect','xgtx_funded_builder','xgtx_bridge_builder']+[name+'_tests' for name in SUITES]
        run('build',['cmake','--build',str(build),'--target',*targets,'-j2'])
        listing=subprocess.run(['ctest','--test-dir',str(build),'--show-only=json-v1'],cwd=root,env=env,text=True,capture_output=True,check=True)
        (output/'ctest-inventory.json').write_text(listing.stdout)
        report['selected_suites']=validate_inventory(json.loads(listing.stdout),build);save()
        run('all-exchange-suites',['ctest','--test-dir',str(build),'--output-on-failure','--no-tests=error','-R','^xgtx_'])
        run('full-unit-suite',['timeout','--kill-after=5s','900s',str(build/'tests/unit_tests/unit_tests'),'--data-dir',str(build/'tests/data')])
        run('interface-http-unit-tests',[sys.executable,'-m','unittest','discover','-s',str(root/'xgtx-trader'),'-p','test_server.py','-v'])
        for label,script in LIFECYCLES:run(label,[sys.executable,'-u',str(root/script)])
        report['status']='passed'
        print('PASS: all required exchange suites and all five daemon lifecycles. Public activation and deployment validation remain unfinished.',flush=True)
    except BaseException as error:
        report['status']='interrupted' if isinstance(error,KeyboardInterrupt) else 'failed';report['error']=str(error);raise
    finally:
        save();print('Result: '+str(output/'result.json'),flush=True)
if __name__=='__main__':
    try:main()
    except (RuntimeError,ValueError,subprocess.CalledProcessError) as error:print('ERROR: '+str(error),file=sys.stderr);sys.exit(1)
