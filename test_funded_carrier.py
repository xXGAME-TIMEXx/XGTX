#!/usr/bin/env python3
"""Launch only a child offline regtest daemon in a fresh temporary directory."""
import json
import socket
import struct
import subprocess
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT=Path.home()/"xgtx-exchange-dev"
BUILD=ROOT/"build/exchange-integration"
DAEMON=BUILD/"bin/xgtxd"
INSPECT=BUILD/"tests/xgtx_node_smoke/xgtx_projection_inspect"
TOOL=BUILD/"tests/xgtx_funded_carrier/xgtx_funded_builder"
LOG=ROOT/"xgtx-funded-carrier-run.log"
checks=0

def check(value,label):
    global checks
    if not value:raise RuntimeError(label)
    checks+=1
    print("PASS:",label,flush=True)

def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1",0))
        return s.getsockname()[1]

with tempfile.TemporaryDirectory(prefix="xgtx-funded-carrier-") as scratch, LOG.open("w") as log:
    scratch=Path(scratch)
    rpc_port=port()
    p2p_port=port()
    while p2p_port==rpc_port:p2p_port=port()
    url=f"http://127.0.0.1:{rpc_port}"
    process=None
    def request(path,payload,timeout=30):
        data=json.dumps(payload).encode()
        req=urllib.request.Request(url+path,data=data,headers={"Content-Type":"application/json"})
        with urllib.request.urlopen(req,timeout=timeout) as reply:return json.load(reply)
    def rpc(method,params=None,timeout=30):
        response=request("/json_rpc",{"jsonrpc":"2.0","id":"xgtx-smoke","method":method,"params":params or {}},timeout)
        if response.get("error"):raise RuntimeError(str(response["error"]))
        result=response["result"]
        if result.get("status","OK")!="OK":raise RuntimeError(str(result))
        return result
    def command(directory):
        return [str(DAEMON),"--xgtx-exchange-regtest","--regtest","--offline","--keep-fakechain",
                "--data-dir",str(directory),"--fixed-difficulty","1","--non-interactive",
                "--rpc-bind-ip","127.0.0.1","--rpc-bind-port",str(rpc_port),"--rpc-ssl","disabled",
                "--p2p-bind-ip","127.0.0.1","--p2p-bind-port",str(p2p_port),"--no-zmq",
                "--disable-dns-checkpoints","--check-updates","disabled","--log-level","1",
                "--log-file",str(scratch/"daemon.log")]
    directory=scratch/"node"
    dbpath=directory/"fake/exchange-v1/lmdb"
    def start():
        global process
        process=subprocess.Popen(command(directory),stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT)
        deadline=time.monotonic()+180
        while time.monotonic()<deadline:
            if process.poll() is not None:raise RuntimeError(f"Daemon exited during startup ({process.returncode}); see {LOG}")
            try:
                info=rpc("get_info",timeout=2)
                if info.get("height",0)>0:return info
            except (OSError,ValueError,KeyError,RuntimeError):pass
            time.sleep(0.25)
        raise RuntimeError(f"Daemon RPC startup timed out; see {LOG}")
    def stop():
        global process
        if process is None:return
        if process.poll() is None:
            try:request("/stop_daemon",{},10)
            except (OSError,ValueError):pass
            try:process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=15)
        process=None
    def projection():
        result=subprocess.run([str(INSPECT),str(dbpath)],check=True,capture_output=True,text=True,timeout=20)
        return json.loads(result.stdout)
    def aligned(label,height=None,assets=0):
        info=rpc("get_info")
        header=rpc("get_last_block_header")["block_header"]
        state=projection()
        check(state["height"]==info["height"],label+": block counts match")
        check(state["tip"]==header["hash"],label+": tips match")
        check(state["assets"]==assets and state["balances"]==(1 if assets else 0),label+": expected token ledger state")
        if height is not None:check(info["height"]==height,label+": expected height")
        return state
    def tool(*args):
        result=subprocess.run([str(TOOL),*map(str,args)],capture_output=True,text=True,timeout=120)
        if result.returncode:raise RuntimeError("Funded builder failed: "+result.stderr[-5000:])
        for line in reversed(result.stdout.splitlines()):
            if line.startswith("{"):return json.loads(line)
        raise RuntimeError("Funded builder returned no JSON: "+result.stdout[-2000:])
    def mine(address,n=1,nonce=0):
        return rpc("generateblocks",{"amount_of_blocks":n,"reserve_size":20,"wallet_address":address,"prev_block":"","starting_nonce":nonce},300)
    def submit(tx):
        return request("/send_raw_transaction",{"tx_as_hex":tx["tx_as_hex"],"do_not_relay":False,"do_sanity_checks":True},60)
    def pool():return request("/get_transaction_pool_hashes",{}).get("tx_hashes",[])
    try:
        check(DAEMON.is_file() and INSPECT.is_file() and TOOL.is_file(),"native executables exist")
        keys=scratch/"fixture.keys"
        address=tool("create",keys)["address"]
        check(keys.stat().st_mode & 0o077==0,"ephemeral fixture keys are private")
        info=start()
        check(info.get("offline") is True,"funded test node offline")
        print("Mining 80 regression blocks to mature the test input ring...",flush=True)
        mine(address,80)
        aligned("mature regression funding",81)
        genesis=rpc("get_block",{"height":0})["block_header"]["hash"]
        funding=scratch/"funding.bin"
        with funding.open("wb") as out:
            out.write(b"XGTXFUND1"+bytes.fromhex(genesis)+struct.pack("<QQI",181,10000000000,16))
            for height in range(1,17):
                block=rpc("get_block",{"height":height})
                txs=request("/get_transactions",{"txs_hashes":[block["miner_tx_hash"]],"decode_as_json":False,"prune":False,"split":False})["txs"]
                check(len(txs)==1 and len(txs[0]["output_indices"])==1,"funding block "+str(height)+": chain output index")
                blob=bytes.fromhex(block["blob"])
                out.write(struct.pack("<QI",txs[0]["output_indices"][0],len(blob))+blob)
        first=tool("build",keys,funding,0,"GAME")
        conflict=tool("build",keys,funding,1,"OTHER")
        check(first["key_image"]!=conflict["key_image"],"conflict uses a different funded native input")
        response=submit(first)
        check(response.get("status")=="OK","native mempool accepts funded signed carrier: "+str(response))
        check(first["hash"] in pool(),"funded carrier stored in native pool")
        response=submit(conflict)
        check(response.get("status")!="OK","same exchange nonce rejected despite different native input")
        rpc("flush_txpool",{"txids":[first["hash"]]})
        check(first["hash"] not in pool(),"pool removal releases reservation")
        response=submit(conflict)
        check(response.get("status")=="OK","previously conflicting carrier accepted after removal: "+str(response))
        rpc("flush_txpool",{"txids":[conflict["hash"]]})
        check(submit(first).get("status")=="OK","original funded carrier reaccepted")
        mine(address,1,100)
        block=rpc("get_block",{"height":81})
        check(first["hash"] in block["tx_hashes"],"template includes and node mines funded carrier")
        issued=aligned("confirmed native-backed fee payment and token issuance",82,1)
        response=submit(conflict)
        check(response.get("status")!="OK","confirmed nonce replay rejected with unspent native input")
        stop();start()
        check(aligned("funded issuance after restart",82,1)==issued,"token state persists across actual daemon restart")
        response=request("/pop_blocks",{"nblocks":1,"keep_txs":False})
        check(response.get("status")=="OK","native carrier block removal succeeds")
        aligned("issuance rollback",81,0)
        check(submit(first).get("status")=="OK","rolled-back native input and exchange nonce spend again")
        mine(address,1,200)
        check(first["hash"] in rpc("get_block",{"height":81})["tx_hashes"],"funded carrier mined again after rollback")
        restored=aligned("remined issuance",82,1)
        stop()
        check(projection()==restored,"funded token state survives final shutdown")
        print(f"{checks} checks passed: chain-funded carrier construction, native mempool nonce conflicts, removal, template/mining, restart and rollback",flush=True)
        print("Regression coins only. Native mining fee paid by real test-chain input; native XGTX DEX deposits/redemptions and production wallet/RPC/UI remain unfinished.",flush=True)
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill();process.wait(timeout=10)
