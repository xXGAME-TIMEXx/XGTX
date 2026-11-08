#!/usr/bin/env python3
import importlib.util,json,os,shutil,subprocess,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
PACKAGE=Path(__file__).resolve().parent
class InstallerTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
  # Installer fixtures use pristine originals explicitly passed by developer.
  source=Path(os.environ.get('XGTX_PHASE34_ORIGINAL','migration-source-current'))
  if not source.exists():self.skipTest('pristine source fixture not provided')
  for rel in json.loads((PACKAGE/'phase34-source-hashes.json').read_text()):
   target=self.root/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source/rel,target)
  (self.root/'tests').mkdir();(self.root/'tests/CMakeLists.txt').write_text('# tests\n')
  for name in ['apply_projection_migration.py','phase34-source-hashes.json','phase34-native-methods.inc','phase34-cmake.inc']:shutil.copy2(PACKAGE/name,self.root/name)
 def tearDown(self):self.temp.cleanup()
 def run_installer(self):return subprocess.run(['python3',str(self.root/'apply_projection_migration.py')],capture_output=True,text=True)
 def test_install_idempotent_no_daemon_change(self):
  self.assertEqual(self.run_installer().returncode,0)
  first={p:p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
  self.assertEqual(self.run_installer().returncode,0)
  self.assertTrue(all(p.read_bytes()==data for p,data in first.items()))
  cpp=(self.root/'src/blockchain_db/lmdb/db_lmdb.cpp').read_text()
  self.assertEqual(cpp.count('void BlockchainLMDB::rehearse_exchange_migration('),1)
  self.assertNotIn('cryptonote_core.cpp',json.loads((PACKAGE/'phase34-source-hashes.json').read_text()))
 def test_mismatch_stops_before_any_source_patch(self):
  header=self.root/'src/blockchain_db/lmdb/db_lmdb.h';header.write_text(header.read_text()+'\n// other change\n')
  before={p:p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
  self.assertNotEqual(self.run_installer().returncode,0)
  self.assertTrue(all(p.read_bytes()==data for p,data in before.items()))
  self.assertFalse(list(self.root.rglob('*.before-phase34-migration')))
class RunnerTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
  spec=importlib.util.spec_from_file_location('migration_runner',PACKAGE/'run_projection_migration.py');self.m=importlib.util.module_from_spec(spec);spec.loader.exec_module(self.m)
  self.m.ROOT=self.root;self.m.BACKUP=self.root/'snapshot/lmdb';self.m.COPY=self.root/'xgtx-mainnet-migration-test-fixture/lmdb';self.m.BUILD=self.root/'build';self.m.REPORT=self.root/'report'
  for folder in [self.m.BACKUP,self.m.COPY]:folder.mkdir(parents=True);(folder/'data.mdb').write_bytes(b'cold native backup')
  tool=self.m.BUILD/'bin/xgtx-projection-migrate';tool.parent.mkdir(parents=True);tool.touch()
  self.calls=[];self.before={'height':642,'genesis':'ab'*32,'tip':'cd'*32,'native_transactions':642,'native_commitment':'ef'*32,'public_activation':False};self.after=dict(self.before)
 def tearDown(self):self.temp.cleanup()
 def fake_run(self,name,args,timeout=3600):
  self.calls.append((name,args))
  if name=='05-read-only-inspection':return 'XGTX_MIGRATION_JSON '+json.dumps(self.before)+'\n'
  if name=='06-copy-migration':
   (self.m.COPY/'data.mdb').write_bytes(b'native plus exchange projection')
   return 'XGTX_MIGRATION_JSON '+json.dumps(self.after)+'\n'
  return ''
 def result(self):return json.loads((self.m.REPORT/'result.json').read_text())
 def test_success_still_not_public_ready(self):
  with patch.object(self.m,'run',side_effect=self.fake_run):self.assertEqual(self.m.main(),0)
  self.assertEqual(self.result()['status'],'passed');self.assertFalse(self.result()['launch_ready']);self.assertTrue(self.result()['backup_unchanged'])
  self.assertIn('--no-tests=error',self.calls[3][1])
 def test_wrong_native_commitment_fails(self):
  self.after['native_commitment']='ff'*32
  with patch.object(self.m,'run',side_effect=self.fake_run):self.assertEqual(self.m.main(),1)
  self.assertEqual(self.result()['status'],'failed')
 def test_wrong_height_never_migrates(self):
  self.before['height']=643
  with patch.object(self.m,'run',side_effect=self.fake_run):self.assertEqual(self.m.main(),1)
  self.assertFalse(any(name=='06-copy-migration' for name,args in self.calls))
 def test_hard_link_rejected_before_build(self):
  (self.m.COPY/'data.mdb').unlink();os.link(self.m.BACKUP/'data.mdb',self.m.COPY/'data.mdb')
  with patch.object(self.m,'run',side_effect=self.fake_run):self.assertEqual(self.m.main(),1)
  self.assertEqual(self.calls,[])
 def test_different_snapshot_rejected_before_build(self):
  (self.m.COPY/'data.mdb').write_bytes(b'different')
  with patch.object(self.m,'run',side_effect=self.fake_run):self.assertEqual(self.m.main(),1)
  self.assertEqual(self.calls,[])
 def test_native_fixture_failure_stops_before_migration(self):
  def fail(name,args,timeout=3600):
   if name=='04-native-migration-tests':raise RuntimeError('fixture failure')
   return self.fake_run(name,args,timeout)
  with patch.object(self.m,'run',side_effect=fail):self.assertEqual(self.m.main(),1)
  self.assertFalse(any(name=='06-copy-migration' for name,args in self.calls));self.assertEqual(self.result()['status'],'failed')
if __name__=='__main__':unittest.main()
