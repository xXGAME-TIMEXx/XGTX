import importlib.util,tempfile,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('builder',Path(__file__).with_name('run_ubuntu24_release.py'));builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
class StagingTests(unittest.TestCase):
 def setUp(self):self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)/'project';self.root.mkdir();self.dest=Path(self.temp.name)/'stage';self.manifest={}
 def tearDown(self):self.temp.cleanup()
 def test_sources_copied_and_hashed_backups_excluded(self):
  p=self.root/'src';p.mkdir();(p/'engine.cpp').write_text('verified source');(p/'engine.cpp.before-fix').write_text('old');(p/'target').mkdir();(p/'target/secret').write_text('cache')
  builder.copy_source(p,self.dest,self.root,self.manifest);self.assertEqual(set(self.manifest),{'src/engine.cpp'});self.assertEqual((self.dest/'engine.cpp').read_text(),'verified source');self.assertFalse((self.dest/'target').exists())
 def test_escaping_symlink_rejected(self):
  external=Path(self.temp.name)/'outside';external.write_text('private');p=self.root/'link';p.symlink_to(external)
  with self.assertRaises(RuntimeError):builder.copy_source(p,self.dest,self.root,self.manifest)
 def test_unexpected_private_wallet_key_rejected(self):
  p=self.root/'founder.keys';p.write_text('private')
  with self.assertRaises(RuntimeError):builder.copy_source(p,self.dest,self.root,self.manifest)
 def test_upstream_public_test_key_allowed(self):
  p=self.root/'tests/data/fixture.keys';p.parent.mkdir(parents=True);p.write_text('upstream public test fixture');builder.copy_source(p,self.dest,self.root,self.manifest);self.assertIn('tests/data/fixture.keys',self.manifest)
 def test_private_rpc_configuration_rejected(self):
  p=self.root/'wallet-rpc-config.json';p.write_text('{}')
  with self.assertRaises(RuntimeError):builder.copy_source(p,self.dest,self.root,self.manifest)
 def test_normal_internal_symlink_materialized(self):
  (self.root/'public.h').write_text('header');p=self.root/'header-link';p.symlink_to('public.h');builder.copy_source(p,self.dest,self.root,self.manifest);self.assertFalse(self.dest.is_symlink());self.assertEqual(self.dest.read_text(),'header')
if __name__=='__main__':unittest.main()
