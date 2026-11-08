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
LOG=ROOT/"xgtx-exchange-lifecycle-run.log"
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

with tempfile.TemporaryDirectory(prefix="xgtx-exchange-lifecycle-") as scratch, LOG.open("w") as log:
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
    ZERO="00"*32
    TREASURY="583e13bbd119abe8c7c773c5f7d6602244ca331eca637441a252fbc130fea58a"
    def balance(state,owner,asset):return sum(row["amount"] for row in state["balances"] if row["owner"]==owner and row["asset"]==asset)
    def reserve(state,asset):return sum(p["reserve_a"] if p["a"]==asset else p["reserve_b"] if p["b"]==asset else 0 for p in state["pools"])
    def economic(state):return {k:v for k,v in state.items() if k not in ("height","tip")}
    def audited(label):
        state=tool("inspect",dbpath)
        info=rpc("get_info");header=rpc("get_last_block_header")["block_header"]
        check(state["height"]==info["height"] and state["tip"]==header["hash"],label+": native/exchange alignment")
        check(all(a["id"]!=ZERO for a in state["assets"]),label+": no fabricated native-asset entry")
        for a in state["assets"]:
            total=sum(row["amount"] for row in state["balances"] if row["asset"]==a["id"])+reserve(state,a["id"])
            check(total==a["supply"],label+": "+a["ticker"]+" supply conserved")
        for p in state["pools"]:
            check(sum(row["amount"] for row in state["lp"] if row["pool"]==p["id"])==p["shares"],label+": LP supply conserved")
        return state
    def encode_op(kind,nonce,asset=ZERO,other=ZERO,recipient=ZERO,quantity=1,limit=0,minimum=0,ticker=""):
        name=ticker.encode("ascii")
        data=(b"XGTXDEX\0\1"+bytes([kind])+bytes.fromhex(genesis)+bytes.fromhex(alice["owner"])+struct.pack("<QQ",nonce,181)+
              bytes.fromhex(asset)+bytes.fromhex(other)+bytes.fromhex(recipient)+struct.pack("<QQQBB",quantity,limit,minimum,0,len(name))+name)
        check(len(data)==212+len(name),"canonical operation size for nonce "+str(nonce))
        return data
    def act(kind,nonce,label,**fields):
        op=scratch/("operation-"+str(nonce)+".bin");op.write_bytes(encode_op(kind,nonce,**fields))
        tx=tool("build-operation",keys,funding,nonce-1,op)
        response=submit(tx);check(response.get("status")=="OK",label+": funded native admission: "+str(response))
        height=rpc("get_info")["height"];mine(alice["address"],1,nonce*100)
        check(tx["hash"] in rpc("get_block",{"height":height})["tx_hashes"],label+": normal template/mining")
        state=audited(label)
        check(next(row["nonce"] for row in state["nonces"] if row["owner"]==alice["owner"])==nonce,label+": confirmed owner nonce")
        return tx,state
    try:
        check(DAEMON.is_file() and TOOL.is_file(),"native executables exist")
        keys=scratch/"alice.keys";alice=tool("create",keys);bob=tool("create",scratch/"bob.keys")
        check(alice["owner"]!=bob["owner"] and alice["owner"]!=TREASURY,"independent test owner, recipient and designated treasury")
        info=start();check(info.get("offline") is True,"lifecycle node offline")
        print("Mining 80 regression blocks for native transaction fees...",flush=True);mine(alice["address"],80)
        genesis=rpc("get_block",{"height":0})["block_header"]["hash"]
        funding=scratch/"funding.bin"
        with funding.open("wb") as out:
            out.write(b"XGTXFUND1"+bytes.fromhex(genesis)+struct.pack("<QQI",181,10000000000,16))
            for height in range(1,17):
                block=rpc("get_block",{"height":height})
                txs=request("/get_transactions",{"txs_hashes":[block["miner_tx_hash"]],"decode_as_json":False,"prune":False,"split":False})["txs"]
                if len(txs)!=1 or len(txs[0]["output_indices"])!=1:raise RuntimeError("Unexpected funding coinbase")
                blob=bytes.fromhex(block["blob"]);out.write(struct.pack("<QI",txs[0]["output_indices"][0],len(blob))+blob)
        _,state=act(1,1,"ALPHA issuance",ticker="ALPHA",quantity=1000000)
        a=next(asset["id"] for asset in state["assets"] if asset["ticker"]=="ALPHA")
        _,state=act(1,2,"BETA issuance",ticker="BETA",quantity=1000000)
        b=next(asset["id"] for asset in state["assets"] if asset["ticker"]=="BETA")
        _,state=act(4,3,"pool creation",asset=a,other=b,quantity=500000,limit=500000)
        check(reserve(state,a)==500000 and reserve(state,b)==500000 and state["pools"][0]["shares"]==500000,"initial on-chain reserves and LP shares")
        _,state=act(5,4,"liquidity addition",asset=a,other=b,quantity=100000,limit=100000,minimum=1)
        check(reserve(state,a)==600000 and reserve(state,b)==600000 and state["pools"][0]["shares"]==600000,"added liquidity reflected in native-persisted state")
        _,state=act(7,5,"swap",asset=a,other=b,quantity=100000,minimum=1)
        swap_out=600000*99700//699700
        check(balance(state,TREASURY,a)==50,"exact 0.05% protocol fee credited to designated public treasury")
        check(reserve(state,a)==600000+99700+250,"exact 0.25% LP fee retained in reserves")
        check(reserve(state,b)==600000-swap_out and balance(state,alice["owner"],b)==400000+swap_out,"on-chain swap output and reserve change")
        _,state=act(6,6,"liquidity removal",asset=a,other=b,quantity=50000,limit=1,minimum=1)
        check(state["pools"][0]["shares"]==550000,"LP shares burned on withdrawal")
        check(reserve(state,a)==699950-699950*50000//600000,"ALPHA proportional withdrawal")
        check(reserve(state,b)==600000-swap_out-(600000-swap_out)*50000//600000,"BETA proportional withdrawal")
        before_burn=economic(state)
        burn,state=act(3,7,"token burn",asset=a,quantity=1000)
        check(next(asset["supply"] for asset in state["assets"] if asset["id"]==a)==999000,"burn reduces actual persisted supply")
        transfer,state=act(2,8,"token transfer",asset=b,recipient=bob["owner"],quantity=10000)
        check(balance(state,bob["owner"],b)==10000,"recipient public token balance credited")
        check(balance(state,TREASURY,a)==50,"protocol fee persists through later operations")
        final=economic(state);stop();start()
        check(economic(audited("restart"))==final,"all token/pool/fee/LP/nonce state survives daemon restart")
        response=request("/pop_blocks",{"nblocks":2,"keep_txs":False});check(response.get("status")=="OK","remove burn and transfer blocks")
        check(economic(audited("two-block undo"))==before_burn,"rollback restores exact prior supplies, balances, LP, pools and nonces")
        for tx,label in ((burn,"burn replay"),(transfer,"transfer replay")):
            check(submit(tx).get("status")=="OK",label+": restored native input/nonce accepted")
            height=rpc("get_info")["height"];mine(alice["address"],1,height*10)
            check(tx["hash"] in rpc("get_block",{"height":height})["tx_hashes"],label+": mined")
        check(economic(audited("deterministic replay"))==final,"replay restores identical economic state and protocol fee")
        stop();check(economic(tool("inspect",dbpath))==final,"final read-only state after shutdown")
        print(f"{checks} checks passed: funded on-chain assets, pool/LP, swap fees to designated treasury, burn/transfer, conservation, restart, rollback and replay",flush=True)
        print("Regression tokens only. Native XGTX DEX backing/redemption and production wallet/RPC/UI remain unfinished.",flush=True)
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill();process.wait(timeout=10)
