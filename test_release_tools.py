import json
import contextlib
import io
from types import SimpleNamespace
import os
from pathlib import Path
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler,HTTPServer
from unittest.mock import patch
import run_release_verification as gate
import xgtx_launch_preflight as preflight

class ReleaseTools(unittest.TestCase):
    def source(self,root):
        def put(name,data):
            p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(data)
        put('src/cryptonote_config.h','#define XGTX_INITIAL_PROTOCOL_VERSION 15\n#define HF_VERSION_VIEW_TAGS 15\n')
        put('src/hardforks/hardforks.cpp','const hardfork_t mainnet_hard_forks[] = {\n { XGTX_INITIAL_PROTOCOL_VERSION, 0, 0, 0 },\n};\n')
        put('src/cryptonote_core/cryptonote_core.cpp','if (exchange_regtest) {native_db->configure_exchange_for_tests(config);}')
        put('src/blockchain_db/lmdb/db_lmdb.cpp','Native/exchange startup state mismatch; use a fresh isolated database')
    def test_public_blockers(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);self.source(root);r=preflight.inventory(root)
            self.assertFalse(r['launch_ready']);self.assertEqual(r['hardfork_tables']['mainnet'],[{'version':15,'block_index':0}])
            codes={x['code'] for x in r['blockers']}
            self.assertNotIn('modern_proof_schedule_missing',codes)
            self.assertEqual(r['view_tags_protocol_version'],15)
            self.assertTrue({'public_activation_unimplemented','existing_chain_migration_unimplemented','portable_release_unverified','live_chain_metadata_missing'}<=codes)
    def test_numeric_future_fork(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);self.source(root);p=root/'src/hardforks/hardforks.cpp';p.write_text(p.read_text().replace('\n};','\n {16, 123456, 0, 0},\n};'))
            r=preflight.inventory(root);self.assertEqual(r['hardfork_tables']['mainnet'][-1],{'version':16,'block_index':123456});self.assertFalse(r['launch_ready'])
    def test_missing_schedule_unknown(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIn('hardfork_schedule_unknown',{x['code'] for x in preflight.inventory(Path(d))['blockers']})
    def test_refuse_unsafe_rpc(self):
        for url in ('http://example.com:48081','http://localhost:48081','https://127.0.0.1:48081','http://a:b@127.0.0.1:48081','http://127.0.0.1:48081/secret','http://127.0.0.1:48081?token=secret','http://127.0.0.1:48081#secret','http://127.0.0.1:0'):
            with self.subTest(url=url),self.assertRaises(ValueError):preflight.rpc_url(url)
    def test_rpc_allowlist(self):
        with self.assertRaises(ValueError):preflight.rpc('http://127.0.0.1:1','start_mining',{})
    def test_no_wallet_or_key_read(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);self.source(root);key=root/'founder.keys';key.write_text('SECRET_MUST_NOT_APPEAR')
            r=preflight.inventory(root);self.assertNotIn('SECRET_MUST_NOT_APPEAR',json.dumps(r));self.assertEqual(key.read_text(),'SECRET_MUST_NOT_APPEAR')
    def test_unavailable_rpc_recorded(self):
        with tempfile.TemporaryDirectory() as d,patch.object(preflight,'rpc',side_effect=OSError('private text')):
            root=Path(d);self.source(root);r=preflight.inventory(root,'http://127.0.0.1:48081');self.assertEqual(r['node']['status'],'unavailable');self.assertNotIn('private text',json.dumps(r))
    def test_observed_metadata_filtered(self):
        replies=[{'height':123,'nettype':'mainnet','offline':False,'unexpected':'PRIVATE'}, {'block_header':{'hash':'a'*64,'height':0,'blob':'PRIVATE'}}, {'version':16,'enabled':False,'unexpected':'PRIVATE'}]
        with tempfile.TemporaryDirectory() as d,patch.object(preflight,'rpc',side_effect=replies) as rpc:
            root=Path(d);self.source(root);r=preflight.inventory(root,'http://127.0.0.1:48081');self.assertEqual(r['node']['info']['height'],123);self.assertNotIn('PRIVATE',json.dumps(r));self.assertEqual([x.args[1] for x in rpc.call_args_list],['get_info','get_block','hard_fork_info'])
    def test_redirect_refused(self):
        with self.assertRaises(ValueError):preflight.NoRedirect().redirect_request(None,None,None,None,None,None)
    def inventory(self,root):
        rows=[]
        for name in gate.SUITES:
            p=root/(name+'_tests');p.write_text('fixture');p.chmod(0o700);rows.append({'name':name,'command':[str(p)]})
        return {'tests':rows}
    def test_all_thirteen_required(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);r=self.inventory(root);self.assertEqual(len(gate.validate_inventory(r,root)),13)
    def test_missing_late_suite_fails(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);r=self.inventory(root);r['tests']=[x for x in r['tests'] if x['name']!='xgtx_native_withdraw']
            with self.assertRaisesRegex(ValueError,'xgtx_native_withdraw'):gate.validate_inventory(r,root)
    def test_unbuilt_executable_fails(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);r=self.inventory(root);Path(r['tests'][-1]['command'][0]).unlink()
            with self.assertRaises(ValueError):gate.validate_inventory(r,root)
    def test_executable_outside_build_fails(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);r=self.inventory(root);r['tests'][0]['command']=['/bin/true']
            with self.assertRaises(ValueError):gate.validate_inventory(r,root)
    def test_duplicate_suite_fails(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);r=self.inventory(root);r['tests'].append(r['tests'][0])
            with self.assertRaises(ValueError):gate.validate_inventory(r,root)
    def simulated_gate(self,home,fail=False):
        root=home/'xgtx-exchange-dev';build=root/'build/exchange-integration';build.mkdir(parents=True)
        data=self.inventory(build)
        for _,script in gate.LIFECYCLES:(root/script).touch()
        for name in ('tests/xgtx_backed_ledger/CMakeLists.txt','xgtx-trader/test_server.py'):
            p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.touch()
        calls=[]
        class Process:
            def __init__(self,command,**kwargs):
                calls.append(command);self.code=1 if fail and command[0]=='ctest' else 0
            def wait(self,timeout):return self.code
        with patch.object(Path,'home',return_value=home),patch.object(gate.subprocess,'Popen',Process),patch.object(gate.subprocess,'run',return_value=SimpleNamespace(stdout=json.dumps(data))),contextlib.redirect_stdout(io.StringIO()):
            if fail:
                with self.assertRaises(RuntimeError):gate.main()
            else:gate.main()
        result=json.loads(next((root/'release-verification').glob('*/result.json')).read_text())
        return calls,result
    def test_full_gate_runs_every_required_lifecycle(self):
        with tempfile.TemporaryDirectory() as d:
            calls,result=self.simulated_gate(Path(d));self.assertEqual(result['status'],'passed');self.assertFalse(result['launch_ready']);self.assertEqual(len(result['selected_suites']),13)
            for _,script in gate.LIFECYCLES:self.assertTrue(any(command[-1].endswith(script) for command in calls))
            self.assertEqual(len(result['steps']),10)
    def test_gate_stops_after_exchange_failure(self):
        with tempfile.TemporaryDirectory() as d:
            calls,result=self.simulated_gate(Path(d),True);self.assertEqual(result['status'],'failed');self.assertEqual(result['steps'][-1]['name'],'all-exchange-suites');self.assertEqual(len(calls),3)
    def test_extra_exchange_suite_included(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);r=self.inventory(root);p=root/'extra';p.write_text('fixture');p.chmod(0o700);r['tests'].append({'name':'xgtx_future','command':[str(p)]});self.assertIn('xgtx_future',gate.validate_inventory(r,root))

if __name__=='__main__':unittest.main()
