#!/usr/bin/env python3
"""Rehearse only against the separately named WSL copy; never start a node."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,subprocess,sys,time
ROOT=Path(__file__).resolve().parent
HOME_DIR=Path.home()
BACKUP=HOME_DIR/'xgtx-mainnet-snapshot-20261015'/'lmdb'
COPY=HOME_DIR/'xgtx-mainnet-migration-test-20261015'/'lmdb'
BUILD=ROOT/'build/exchange-integration'
REPORT=ROOT/'migration-verification'/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()

def check(value,why):
    if not value:raise RuntimeError(why)
    print('PASS: '+why,flush=True)

def run(name,args,timeout=3600):
    log=REPORT/(name+'.log')
    print(f'Running {name}; log: {log}',flush=True)
    start=time.monotonic();heartbeat=start
    with log.open('w') as output:
        process=subprocess.Popen([str(x) for x in args],cwd=ROOT,stdout=output,stderr=subprocess.STDOUT,start_new_session=True)
        try:
            while process.poll() is None:
                now=time.monotonic()
                if now-start>timeout:raise RuntimeError(f'{name} timed out')
                if now-heartbeat>=15:
                    lines=log.read_text(errors='replace').splitlines()
                    print(f'{name}: {int(now-start)}s; '+(lines[-1][-220:] if lines else 'working'),flush=True);heartbeat=now
                time.sleep(1)
        except BaseException:
            import signal
            os.killpg(process.pid,signal.SIGTERM)
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.wait()
            raise
    if process.returncode:
        print('\n'.join(log.read_text(errors='replace').splitlines()[-50:]),flush=True)
        raise RuntimeError(f'{name} failed; see {log}')
    print('PASS: '+name,flush=True)
    return log.read_text(errors='replace')

def inventory(text):
    rows=[line.split('XGTX_MIGRATION_JSON ',1)[1] for line in text.splitlines() if line.startswith('XGTX_MIGRATION_JSON ')]
    check(len(rows)==1,'single native inventory returned')
    return json.loads(rows[0])

def main():
    REPORT.mkdir(parents=True)
    result={'status':'failed','launch_ready':False,'public_activation':False,'report':str(REPORT)}
    backup_hash=None
    try:
        for folder in (BACKUP,COPY):
            check(folder.is_dir() and (folder/'data.mdb').is_file(),'existing local database: '+str(folder))
            check(not folder.is_symlink() and not (folder/'data.mdb').is_symlink(),'database path is not a symlink: '+str(folder))
        check(BACKUP.resolve()!=COPY.resolve(),'backup and rehearsal directories differ')
        check(not os.path.samefile(BACKUP/'data.mdb',COPY/'data.mdb'),'rehearsal data is not a hard link to backup')
        backup_hash=digest(BACKUP/'data.mdb')
        check(digest(COPY/'data.mdb')==backup_hash,'rehearsal starts byte-identical to untouched backup')
        result['backup_sha256']=backup_hash
        run('01-standalone-migration-tests',['bash','tests/xgtx_projection_migration/run-tests.sh'])
        run('02-configure',['cmake','-S','.', '-B',BUILD,'-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTS=ON','-DSTATIC=OFF'])
        run('03-build',['cmake','--build',BUILD,'--target','xgtx_projection_migrate','xgtx_projection_migration_tests','-j2'])
        run('04-native-migration-tests',['ctest','--test-dir',BUILD,'-R','^xgtx_projection_migration$','--no-tests=error','--output-on-failure','-V'])
        tool=BUILD/'bin/xgtx-projection-migrate'
        check(tool.is_file(),'native migration executable exists')
        before=inventory(run('05-read-only-inspection',[tool,'inspect',COPY],timeout=120))
        check(before['height']==642,'snapshot contains the expected 642 native blocks')
        check(len(before['genesis'])==64 and len(before['tip'])==64,'complete native chain hashes returned')
        check(before['public_activation'] is False,'inspection does not activate public exchange')
        # Temporary rehearsal policy. Never use this height to deploy a public fork.
        activation=before['height']+1;result['rehearsal_activation_count']=activation
        after=inventory(run('06-copy-migration',[tool,'rehearse',COPY,str(before['height']),before['genesis'],before['tip'],str(activation)],timeout=300))
        check(before==after,'every native table commitment, chain identity and transaction count preserved')
        check(digest(BACKUP/'data.mdb')==backup_hash,'untouched backup unchanged')
        result.update(status='passed',before=before,after=after)
        print('PASS: copied mainnet migration rehearsal. Public activation and release validation remain unfinished.',flush=True)
    except Exception as error:
        result['error']=str(error);print('ERROR: '+str(error),file=sys.stderr,flush=True)
    finally:
        if backup_hash is not None:
            result['backup_unchanged']=digest(BACKUP/'data.mdb')==backup_hash
            if not result['backup_unchanged']:result['status']='failed'
        (REPORT/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        print('Result: '+str(REPORT/'result.json'),flush=True)
    return 0 if result['status']=='passed' else 1
if __name__=='__main__':sys.exit(main())
