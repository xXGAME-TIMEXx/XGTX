import importlib.util,json,hashlib,shutil,subprocess,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
PACKAGE=Path(__file__).resolve().parent
sys.path.insert(0,str(PACKAGE.parent/'phase34-package'))
class InstallerTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name);shutil.copytree(PACKAGE,self.root,dirs_exist_ok=True)
  baseline=PACKAGE.parent/'phase34-installer-validation'
  for rel,expected in json.loads((PACKAGE/'phase35-source-hashes.json').read_text()).items():
   if expected is None:continue
   source_candidates=[Path('migration-source-current/CMakeLists.txt') if rel=='CMakeLists.txt' else baseline/rel,PACKAGE/(rel+'.before-phase35-public-candidate'),PACKAGE/rel]
   source=next((p for p in source_candidates if p.is_file() and hashlib.sha256(p.read_bytes()).hexdigest()==expected),None)
   if source is None:self.temp.cleanup();self.skipTest('pristine phase34 source fixture unavailable')
   dest=self.root/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest)
  (self.root/'tests/CMakeLists.txt').write_text('add_subdirectory(xgtx_projection_migration)\n')
 def tearDown(self):self.temp.cleanup()
 def invoke(self):return subprocess.run(['python3',str(self.root/'apply_public_candidate.py')],capture_output=True)
 def test_idempotent(self):
  self.assertEqual(self.invoke().returncode,0);first={p:p.read_bytes() for p in self.root.rglob('*') if p.is_file()};self.assertEqual(self.invoke().returncode,0);self.assertTrue(all(p.read_bytes()==v for p,v in first.items()))
 def test_source_mismatch_does_not_partially_patch(self):
  p=self.root/'src/hardforks/hardforks.cpp';p.write_text(p.read_text()+'\n// newer source\n');before={p:p.read_bytes() for p in self.root.rglob('*') if p.is_file()};self.assertNotEqual(self.invoke().returncode,0);self.assertTrue(all(p.read_bytes()==v for p,v in before.items()));self.assertFalse(list(self.root.rglob('*.before-phase35-public-candidate')))
class RunnerTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name);spec=importlib.util.spec_from_file_location('candidate',PACKAGE/'run_public_candidate.py');self.m=importlib.util.module_from_spec(spec);spec.loader.exec_module(self.m)
  self.m.ROOT=self.root;self.m.BUILD=self.root/'build';self.m.REPORT=self.root/'report';self.m.BACKUP=self.root/'snapshot';self.m.CANDIDATE=self.root/'copy';self.m.BACKUP.mkdir();(self.m.BACKUP/'lmdb').mkdir();(self.m.BACKUP/'lmdb/data.mdb').write_bytes(b'cold backup');self.m.EXPECTED_SHA=hashlib.sha256(b'cold backup').hexdigest();self.calls=[]
  self.inventory={'height':642,'genesis':self.m.EXPECTED_GENESIS,'tip':self.m.EXPECTED_TIP,'native_commitment':'ab'*32,'native_transactions':642,'public_activation':False}
 def tearDown(self):self.temp.cleanup()
 def fake_run(self,name,args,timeout=3600):
  self.calls.append(name)
  return 'XGTX_MIGRATION_JSON '+json.dumps(self.inventory)+'\n' if 'inventory' in name else ''
 def state(self):return {'exchange':{'height':642},'fork':{'version':15}}
 def result(self):return json.loads((self.m.REPORT/'result.json').read_text())
 def test_success_is_not_deployment_approval(self):
  with patch.object(self.m,'run',side_effect=self.fake_run),patch.object(self.m,'start_and_check',side_effect=lambda _:self.state()):self.assertEqual(self.m.main(),0)
  self.assertFalse(self.result()['launch_ready']);self.assertFalse(self.result()['public_deployment']);self.assertTrue(self.result()['backup_unchanged'])
 def test_full_gate_failure_prevents_copy_and_startup(self):
  def fail(name,args,timeout=3600):
   if name=='08-full-release-gate':raise RuntimeError('regression failure')
   return self.fake_run(name,args,timeout)
  with patch.object(self.m,'run',side_effect=fail),patch.object(self.m,'start_and_check') as start:self.assertEqual(self.m.main(),1);start.assert_not_called()
  self.assertFalse(self.m.CANDIDATE.exists())
 def test_existing_copy_never_overwritten(self):
  self.m.CANDIDATE.mkdir()
  with patch.object(self.m,'run',side_effect=self.fake_run):self.assertEqual(self.m.main(),1)
  self.assertEqual(self.calls,[])
 def test_backup_mismatch_fails_before_build(self):
  (self.m.BACKUP/'lmdb/data.mdb').write_bytes(b'changed')
  with patch.object(self.m,'run',side_effect=self.fake_run):self.assertEqual(self.m.main(),1)
  self.assertEqual(self.calls,[])
if __name__=='__main__':unittest.main()
