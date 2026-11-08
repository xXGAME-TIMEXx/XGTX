#!/usr/bin/env python3
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,re,shutil,subprocess,sys,time
ROOT=Path(__file__).resolve().parent
IMAGE='ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55'
EXPECTED_RUST='rustc 1.99.0 (b940084d7 2026-09-28)'
EXPECTED_SHA='69d876294a27e5c0346d61e6ee23e899e5b6184dcaab1a1458eb75d1f5801605'
SOURCE_DIRS=['src','cmake','external','contrib','tests','translations','utils/python-rpc']
ROOT_NAMES=['CMakeLists.txt', 'Makefile', 'LICENSE', 'README.md', 'run_release_verification.py', 'run_projection_migration.py', 'run_public_candidate.py', 'test_node_smoke.py', 'test_funded_carrier.py', 'test_exchange_lifecycle.py', 'test_wallet_bootstrap.py', 'test_loopback_network.py', 'run_ubuntu24_release.py', 'xgtx_launch_preflight.py']

def digest(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for part in iter(lambda:f.read(1024*1024),b''):h.update(part)
 return h.hexdigest()
def ensure(value,why):
 if not value:raise RuntimeError(why)
 print('PASS: '+why,flush=True)
def copy_source(source,dest,root,manifest):
 resolved=source.resolve();ensure_inside=resolved.is_relative_to(root.resolve())
 if not ensure_inside:raise RuntimeError('Source symlink escapes project: '+str(source))
 if source.name in {'.git','__pycache__','target','node_modules'} or '.before-' in source.name or source.name.endswith(('.pyc','.log')):return
 if source.is_dir():
  dest.mkdir(parents=True,exist_ok=True)
  for child in sorted(source.iterdir()):copy_source(child,dest/child.name,root,manifest)
 elif source.is_file():
  rel=source.relative_to(root).as_posix()
  if source.suffix=='.keys' and not rel.startswith('tests/data/'):raise RuntimeError('Unexpected key file in source selection: '+rel)
  if source.name=='.env' or (source.suffix=='.json' and re.search(r'(password|credential|wallet.rpc.config)',source.name,re.I)):raise RuntimeError('Unexpected private configuration in source selection: '+rel)
  dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest);manifest[rel]=digest(dest)
 else:raise RuntimeError('Nonregular source input: '+str(source))
def supervise(args,log):
 with log.open('w') as output:
  process=subprocess.Popen(args,stdout=output,stderr=subprocess.STDOUT,start_new_session=False)
  start=last=time.monotonic()
  try:
   while process.poll() is None:
    now=time.monotonic()
    if now-last>=15:
     with log.open('rb') as stream:
      stream.seek(max(0,log.stat().st_size-4096));lines=stream.read().decode(errors='replace').splitlines()
     print(f'Ubuntu24 build: {int(now-start)}s; '+(lines[-1][-240:] if lines else 'working'),flush=True);last=now
    time.sleep(1)
  except BaseException:
   subprocess.run(['sudo','docker','stop','-t','20',CONTAINER_NAME],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);process.wait(timeout=30);raise
 if process.returncode:
  print('\n'.join(log.read_text(errors='replace').splitlines()[-50:]),flush=True);raise RuntimeError('Container build/test failed; see '+str(log))
CONTAINER_NAME=''
def main():
 global CONTAINER_NAME
 stamp=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ');out=ROOT/'ubuntu24-releases'/stamp
 out.mkdir(parents=True,mode=0o700);result={'status':'failed','launch_ready':False,'server_deployed':False,'output':str(out),'image':IMAGE}
 backup=Path.home()/'xgtx-mainnet-snapshot-20261015/lmdb/data.mdb'
 try:
  ensure(os.getuid()!=0,'run builder as dyllon; sudo is used only for Docker')
  ensure(backup.is_file() and not backup.is_symlink() and digest(backup)==EXPECTED_SHA,'untouched snapshot SHA256 matches verified backup')
  reports=sorted((ROOT/'public-candidate-verification').glob('*/result.json'),reverse=True)
  passed=next((p for p in reports if json.loads(p.read_text()).get('status')=='passed'),None)
  ensure(passed is not None,'passing native public-candidate report exists');result['candidate_report']=str(passed)
  candidate=json.loads(passed.read_text());ensure(candidate.get('backup_unchanged') is True and candidate.get('activation_native_index')==700,'candidate report confirms backup and activation policy')
  for rel in json.loads((ROOT/'phase35-source-hashes.json').read_text()):
   ensure((ROOT/rel).read_bytes()==(ROOT/'phase35-updates'/rel).read_bytes(),'source matches candidate payload: '+rel)
  rust_version=subprocess.check_output(['rustc','--version'],text=True).strip();ensure(rust_version==EXPECTED_RUST,'tested Rust1.99.0 toolchain selected')
  rust=Path(subprocess.check_output(['rustc','--print','sysroot'],text=True).strip()).resolve()
  ensure((rust/'bin/rustc').is_file() and (rust/'bin/cargo').is_file(),'complete Rust sysroot available for read-only mount')
  node=(ROOT/'tests/unit_tests/node_server.cpp').read_text()
  ensure('if (hardfork > HF_VERSION_VIEW_TAGS)' in node and 'cryptonote::txout_to_tagged_key{}' in node,'protocol-aware race-test coinbase fixture is installed')
  tcp=(ROOT/'tests/unit_tests/epee_boosted_tcp_server.cpp').read_text()
  section=tcp[tcp.index('TEST(boosted_tcp_server, write_failure)'):]
  ensure('ASSERT_TRUE(accepted);' in section and 'ASSERT_TRUE(connected);' in section,'write-failure fixture checks actual connection completions')
  ensure("ledger.get('assets', [])==[]" in (ROOT/'run_public_candidate.py').read_text(),'empty RPC collection assertion fix is installed')
  # Only source directories and allowlisted root scripts are staged. No home
  # wallet, RPC configuration, build cache or chain data enters the source pack.
  stage=out/'input';project=stage/'project';project.mkdir(parents=True);manifest={}
  for name in SOURCE_DIRS:
   if (ROOT/name).exists():copy_source(ROOT/name,project/name,ROOT,manifest)
  for name in ROOT_NAMES:
   ensure((ROOT/name).is_file(),'required source input: '+name);copy_source(ROOT/name,project/name,ROOT,manifest)
  for p in sorted(ROOT.glob('test_*.py')):
   if p.name not in ROOT_NAMES:copy_source(p,project/p.name,ROOT,manifest)
  interface=project/'xgtx-trader';interface.mkdir()
  for name in ['server.py','index.html','app.mjs','amounts.mjs','style.css','test_server.py','test_amounts.mjs']:
   source=ROOT/'xgtx-trader'/name;ensure(source.is_file(),'local interface source: '+name);copy_source(source,interface/name,ROOT,manifest)
  gate=project/'run_release_verification.py';text=gate.read_text()
  old="run('full-unit-suite',[str(build/'tests/unit_tests/unit_tests'),'--data-dir',str(build/'tests/data')])"
  new="run('full-unit-suite',['timeout','--kill-after=5s','900s',str(build/'tests/unit_tests/unit_tests'),'--data-dir',str(build/'tests/data')])"
  ensure(text.count(old)==1,'known full-unit gate receives a 15-minute failure timeout')
  gate.write_text(text.replace(old,new,1));manifest['run_release_verification.py']=digest(gate)
  shutil.copytree(ROOT/'release',stage/'release')
  (stage/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
  (out/'work').mkdir();CONTAINER_NAME='xgtx-release-'+stamp.lower()
  result['rust_version']=rust_version;result['source_files']=len(manifest)
  args=['sudo','docker','run','--rm','--init','--name',CONTAINER_NAME,'--platform','linux/amd64',
    '--mount',f'type=bind,src={stage},dst=/source,readonly','--mount',f'type=bind,src={out},dst=/output',
    '--mount',f'type=bind,src={out/"work"},dst=/root/xgtx-exchange-dev',
    '--mount',f'type=bind,src={rust},dst=/rust-toolchain,readonly',
    '--mount',f'type=bind,src={backup.parent},dst=/snapshot,readonly',
    '-e',f'XGTX_UID={os.getuid()}','-e',f'XGTX_GID={os.getgid()}',IMAGE,'bash','/source/release/inside-build.sh']
  subprocess.run(['sudo','-v'],check=True)
  print('Container log: '+str(out/'container.log'),flush=True);supervise(args,out/'container.log')
  release_report=json.loads((out/'release-report.json').read_text());ensure(release_report.get('status')=='passed','Ubuntu24 native build/tests and baseline CPU validation passed')
  ensure(digest(backup)==EXPECTED_SHA,'untouched host backup unchanged')
  for name in ['runtime.tar.gz','source.tar.gz']:
   ensure((out/name).is_file(),'release artifact exists: '+name)
  result.update(status='passed',release=release_report)
 except Exception as error:result['error']=str(error);print('ERROR: '+str(error),file=sys.stderr,flush=True)
 finally:
  result['backup_unchanged']=backup.is_file() and digest(backup)==EXPECTED_SHA
  if not result['backup_unchanged']:result['status']='failed'
  (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('Result: '+str(out/'result.json'),flush=True)
 return 0 if result['status']=='passed' else 1
if __name__=='__main__':sys.exit(main())
