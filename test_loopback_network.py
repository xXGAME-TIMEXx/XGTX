#!/usr/bin/env python3
"""Launch only a child offline regtest daemon in a fresh temporary directory."""
import json
import importlib.util
import os
import threading
import urllib.error
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
LOG=ROOT/"xgtx-network-regtest-a.log"
BRIDGE=BUILD/"tests/xgtx_backed_daemon/xgtx_bridge_builder"
WALLET=BUILD/"bin/xgtx-wallet-rpc"
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

with tempfile.TemporaryDirectory(prefix="xgtx-backed-lifecycle-") as scratch, LOG.open("w") as log:
    scratch=Path(scratch)
    rpc_port=port()
    p2p_port=port()
    while p2p_port==rpc_port:p2p_port=port()
    url=f"http://127.0.0.1:{rpc_port}"
    process=None
    interface=None
    interface_thread=None
    demo_stop=threading.Event()
    def ui(route,payload=None):
        req=urllib.request.Request(f"http://127.0.0.1:{interface.server_port}/api/"+route,json.dumps(payload or {}).encode(),{"Content-Type":"application/json","Origin":f"http://127.0.0.1:{interface.server_port}","Authorization":"Bearer "+interface.token})
        try:
            with urllib.request.urlopen(req,timeout=150) as reply:return json.load(reply)
        except urllib.error.HTTPError as error:raise RuntimeError("Trader interface: "+error.read().decode()) from error
    def ui_preview(payload):
        clean={k:v for k,v in payload.items() if k!="do_not_relay"}
        return ui("preview",{**clean,"expected_owner":alice["owner"],"expected_chain":genesis})
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
        args=[str(DAEMON),"--xgtx-exchange-regtest","--xgtx-backing-regtest","--regtest","--xgtx-loopback-regtest","--keep-fakechain","--allow-local-ip","--add-exclusive-node",f"127.0.0.1:{b_p2p}",
                "--data-dir",str(directory),"--fixed-difficulty","1","--non-interactive",
                "--rpc-bind-ip","127.0.0.1","--rpc-bind-port",str(rpc_port),"--rpc-ssl","disabled",
                "--p2p-bind-ip","127.0.0.1","--p2p-bind-port",str(p2p_port),"--no-zmq",
                "--disable-dns-checkpoints","--check-updates","disabled","--log-level","1",
                "--log-file",str(scratch/"daemon.log")]
        if partitioned:args.append("--offline")
        return args
    directory=scratch/"node"
    dbpath=directory/"fake/exchange-linked-v3/lmdb"
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
    def invoke(binary,*args):
        result=subprocess.run([str(binary),*map(str,args)],capture_output=True,text=True,timeout=120)
        if result.returncode:raise RuntimeError("Funded builder failed: "+result.stderr[-5000:])
        for line in reversed(result.stdout.splitlines()):
            if line.startswith("{"):return json.loads(line)
        raise RuntimeError("Funded builder returned no JSON: "+result.stdout[-2000:])
    def tool(*args):return invoke(TOOL,*args)
    def bridge(*args):return invoke(BRIDGE,*args)
    def mine(address,n=1,nonce=0):
        return rpc("generateblocks",{"amount_of_blocks":n,"reserve_size":20,"wallet_address":address,"prev_block":"","starting_nonce":nonce},300)
    def submit(tx):
        return request("/send_raw_transaction",{"tx_as_hex":tx["tx_as_hex"],"do_not_relay":False,"do_sanity_checks":True},60)
    def pool():return request("/get_transaction_pool_hashes",{}).get("tx_hashes",[])
    ZERO="00"*32
    TREASURY="583e13bbd119abe8c7c773c5f7d6602244ca331eca637441a252fbc130fea58a"
    COIN=10_000_000_000
    wallet_process=None
    wallet_port=port()
    while wallet_port in (rpc_port,p2p_port):wallet_port=port()
    def wallet_rpc(method,params=None,timeout=120):
        data=json.dumps({"jsonrpc":"2.0","id":"backing-wallet","method":method,"params":params or {}}).encode()
        req=urllib.request.Request(f"http://127.0.0.1:{wallet_port}/json_rpc",data=data,headers={"Content-Type":"application/json"})
        with urllib.request.urlopen(req,timeout=timeout) as reply:response=json.load(reply)
        if response.get("error"):raise RuntimeError("Wallet RPC: "+str(response["error"]))
        return response["result"]
    def start_rpc_wallet(keyfile,label):
        global wallet_process,wallet_port
        wallet_port=port()
        while wallet_port in (rpc_port,p2p_port):wallet_port=port()
        params=scratch/(label+"-private-restore.json");bridge("restore-file",keyfile,params)
        check(params.stat().st_mode & 0o077==0,"temporary restore parameters private")
        wallet_dir=scratch/(label+"-wallets");reopen=wallet_dir.exists();wallet_dir.mkdir(mode=0o700,exist_ok=True)
        wallet_log=(scratch/(label+"-wallet.log")).open("w")
        args=[str(WALLET),"--wallet-dir",str(wallet_dir),"--rpc-bind-ip","127.0.0.1","--rpc-bind-port",str(wallet_port),"--disable-rpc-login","--rpc-ssl","disabled","--daemon-address",f"127.0.0.1:{rpc_port}","--daemon-ssl","disabled","--trusted-daemon","--log-level","0","--log-file",str(scratch/(label+"-wallet-internal.log"))]
        wallet_process=subprocess.Popen(args,stdin=subprocess.DEVNULL,stdout=wallet_log,stderr=subprocess.STDOUT)
        deadline=time.monotonic()+120
        while True:
            if wallet_process.poll() is not None:raise RuntimeError("Temporary wallet RPC exited; "+(scratch/(label+"-wallet.log")).read_text()[-3000:])
            try:wallet_rpc("get_version",timeout=2);break
            except (OSError,ValueError,RuntimeError):
                if time.monotonic()>deadline:raise RuntimeError("Temporary wallet RPC startup timed out")
                time.sleep(0.25)
        restore=json.loads(params.read_text());wallet_rpc("open_wallet",{"filename":"payee","password":""}) if reopen else wallet_rpc("generate_from_keys",restore);del restore;params.unlink()
        return wallet_log
    def balance(state,owner,asset):return sum(row["amount"] for row in state["balances"] if row["owner"]==owner and row["asset"]==asset)
    def reserves(state,asset):return sum(p["reserve_a"] if p["a"]==asset else p["reserve_b"] if p["b"]==asset else 0 for p in state["pools"])
    def economic(state):return {k:v for k,v in state.items() if k not in ("height","tip")}
    def audited(label):
        state=bridge("inspect",dbpath)
        public=rpc("xgtx_get_exchange",{"owner":alice["owner"],"limit":1})
        check(public["tip"]==state["tip"] and public["height"]==state["height"],label+": RPC snapshot matches native DB")
        check(public["native_locked"]==str(state["native_locked"]) and public["native_backing"] is True,label+": RPC exact backing amount")
        check(public["treasury"]==TREASURY and public["protocol_fee_bps"]==5 and public["lp_fee_bps"]==25,label+": RPC discloses treasury and fees")
        nonce=next((n["nonce"] for n in state["nonces"] if n["owner"]==alice["owner"]),0)
        check(public["next_nonce"]==str(nonce+1),label+": RPC next confirmed owner nonce")
        assets=list(public.get("assets",[]));cursor=public["next_asset"];seen=set()
        while cursor:
            check(cursor not in seen,label+": asset cursor progresses");seen.add(cursor)
            page=rpc("xgtx_get_exchange",{"owner":alice["owner"],"after_asset":cursor,"limit":1})
            check(page["tip"]==public["tip"] and len(page.get("assets",[]))<=1,label+": stable bounded asset page")
            assets.extend(page.get("assets",[]));cursor=page["next_asset"]
        check([a["id"] for a in assets]==[a["id"] for a in state["assets"]],label+": RPC complete asset pagination")
        for asset in assets:
            raw=next(a for a in state["assets"] if a["id"]==asset["id"])
            check(asset["supply"]==str(raw["supply"]) and asset["balance"]==str(balance(state,alice["owner"],asset["id"])),label+": RPC exact asset supply and owner balance")
        check(len(public.get("pools",[]))==len(state["pools"]),label+": RPC pool page")
        for pool_row in public.get("pools",[]):
            raw=next(p for p in state["pools"] if p["id"]==pool_row["id"])
            check(pool_row["reserve_a"]==str(raw["reserve_a"]) and pool_row["reserve_b"]==str(raw["reserve_b"]) and pool_row["shares"]==str(raw["shares"]),label+": RPC exact pool reserves and shares")
        info=rpc("get_info");header=rpc("get_last_block_header")["block_header"]
        check(state["height"]==info["height"] and state["tip"]==header["hash"],label+": native/exchange alignment")
        native=next((a for a in state["assets"] if a["id"]==ZERO),None)
        check(state["native_locked"]==(native["supply"] if native else 0),label+": reserve liabilities equal native supply")
        for asset in state["assets"]:
            total=sum(b["amount"] for b in state["balances"] if b["asset"]==asset["id"])+reserves(state,asset["id"])
            check(total==asset["supply"],label+": "+asset["ticker"]+" supply conserved")
        for pool_state in state["pools"]:
            check(sum(row["amount"] for row in state["lp"] if row["pool"]==pool_state["id"])==pool_state["shares"],label+": LP shares conserved")
        if connected:
            wait_equal()
            other=bridge("inspect",b_dbpath)
            check(state==other,label+": both native databases have identical complete exchange state")
            check(rpc("xgtx_get_exchange",{"owner":alice["owner"],"limit":100})==b_rpc("xgtx_get_exchange",{"owner":alice["owner"],"limit":100}),label+": peer RPC exposes identical ledger, pool, reserve count and owner nonce")
        return state
    def encode_op(kind,nonce,asset=ZERO,other=ZERO,recipient=ZERO,quantity=1,limit=0,minimum=0,ticker=""):
        name=ticker.encode("ascii")
        return (b"XGTXDEX\0\1"+bytes([kind])+bytes.fromhex(genesis)+bytes.fromhex(alice["owner"])+struct.pack("<QQ",nonce,181)+
                bytes.fromhex(asset)+bytes.fromhex(other)+bytes.fromhex(recipient)+struct.pack("<QQQBB",quantity,limit,minimum,0,len(name))+name)
    def confirm(tx,label):
        if tx.get("ui_draft"):
            result=ui("submit",{"draft_id":tx.pop("ui_draft")})
            check(result["tx_hash"]==tx["hash"],label+": interface relays exact reviewed hash")
            tx["already_relayed"]=True
        if tx.get("already_relayed"):check(tx["hash"] in pool(),label+": wallet-relayed native admission")
        else:
            response=submit(tx);check(response.get("status")=="OK",label+": native mempool admission: "+str(response))
        check(tx["hash"] in pool(),label+": stored in native mempool")
        height=rpc("get_info")["height"]
        if connected:
            wait("transaction P2P relay to node B",lambda:tx["hash"] in b_request("/get_transaction_pool_hashes",{}).get("tx_hashes",[]),seconds=300)
            mine_b(1,height*100);wait_equal()
        else:mine(alice["address"],1,height*100)
        check(tx["hash"] in rpc("get_block",{"height":height})["tx_hashes"],label+": normal template/mining")
        tx["already_relayed"]=False
        return audited(label)
    def act(kind,nonce,label,**fields):
        wallet_rpc("refresh")
        payload={"kind":kind,"quantity":"1","max_fee":str(COIN),"do_not_relay":True,**fields}
        for field in ("quantity","limit","minimum"): 
            if field in payload:payload[field]=str(payload[field])
        preview=economic(bridge("inspect",dbpath))
        result=ui_preview(payload)
        check(result["draft_id"] and result["relayed"] is False,label+": retained native wallet preview")
        draft_id=result["draft_id"]
        if not payload["do_not_relay"]:
            receipt=ui("submit",{"draft_id":draft_id})
            check(receipt["tx_hash"]==result["tx_hash"] and receipt["fee"]==result["fee"],label+": reviewed bytes and fee unchanged at relay")
            result["relayed"]=True

        check(result["owner"]==alice["owner"] and result["chain"]==genesis and result["nonce"]==str(nonce),label+": wallet binds owner, chain and next nonce")
        check(0<int(result["fee"])<=COIN and int(result["weight"])>0,label+": wallet estimates bounded native fee")
        tx={"hash":result["tx_hash"],"tx_as_hex":result["tx_blob"],"already_relayed":result["relayed"],"asset_id":result["asset_id"],"wallet_fee":result["fee"],"ui_draft":draft_id if payload["do_not_relay"] else None}
        check(economic(bridge("inspect",dbpath))==preview,label+": wallet signing never changes confirmed balances")
        if payload["do_not_relay"]:check(tx["hash"] not in pool(),label+": preview does not relay")
        else:check(tx["hash"] in pool(),label+": wallet RPC relays operation")
        state=confirm(tx,label)
        if kind==1:check(result["asset_id"]==next(a["id"] for a in state["assets"] if a["ticker"]==fields["ticker"]),"wallet returns actual confirmed token ID")
        return tx,state
    def withdraw(nonce,quantity,label,relay=False):
        wallet_rpc("refresh")
        before=economic(bridge("inspect",dbpath))
        result=ui_preview({"kind":9,"quantity":str(quantity),"max_fee":str(COIN),"recipient":bob["address"]})
        if relay:
            receipt=ui("submit",{"draft_id":result["draft_id"]})
            check(receipt["tx_hash"]==result["tx_hash"] and receipt["fee"]==result["fee"],label+": exact reviewed withdrawal relayed")
            check(ui("submit",{"draft_id":result["draft_id"]})["tx_hash"]==receipt["tx_hash"],label+": repeat confirmation does not broadcast another transaction")
            result["relayed"]=True
        check(result["owner"]==alice["owner"] and result["chain"]==genesis and result["nonce"]==str(nonce),label+": wallet binds confirmed owner and nonce")
        check(0<int(result["fee"])<=COIN and int(result["payout"])+int(result["fee"])==quantity,label+": exact disclosed payout plus estimated fee")
        check(result["relayed"]==relay and (result["tx_hash"] in pool())==relay,label+": preview/relay flag honored")
        check(economic(bridge("inspect",dbpath))==before,label+": construction never changes confirmed ledger")
        tx={"hash":result["tx_hash"],"tx_as_hex":result["tx_blob"],"already_relayed":relay,"ui_draft":None if relay else result["draft_id"]}
        return result,tx
    b_rpc_port=port();b_p2p=port()
    while len({rpc_port,p2p_port,b_rpc_port,b_p2p})!=4:b_rpc_port=port();b_p2p=port()
    b_directory=scratch/"node-b";b_dbpath=b_directory/"fake/exchange-linked-v3/lmdb"
    b_process=None;connected=False;partitioned=False
    b_log=(ROOT/"xgtx-network-regtest-b.log").open("w")
    def b_request(path,payload,timeout=30):
        req=urllib.request.Request(f"http://127.0.0.1:{b_rpc_port}"+path,json.dumps(payload).encode(),{"Content-Type":"application/json"})
        with urllib.request.urlopen(req,timeout=timeout) as response:return json.load(response)
    def b_rpc(method,params=None):
        result=b_request("/json_rpc",{"jsonrpc":"2.0","id":"node-b","method":method,"params":params or {}})
        if result.get("error"):raise RuntimeError(str(result["error"]))
        return result["result"]
    def wait(label,predicate,seconds=180):
        print("Waiting:",label,flush=True);end=time.monotonic()+seconds;last=None;next_status=time.monotonic()+15
        while time.monotonic()<end:
            for child in (process,b_process):
                if child is not None and child.poll() is not None:raise RuntimeError(label+": owned node exited")
            try:
                result=predicate()
                if result:return result
            except (OSError,ValueError,KeyError,RuntimeError) as error:last=str(error)
            if time.monotonic()>next_status:print("Still waiting:",label,flush=True);next_status=time.monotonic()+15
            time.sleep(0.25)
        raise RuntimeError(label+": timeout; "+str(last))
    def wait_equal():
        return wait("matching peer chain tips",lambda:rpc("get_last_block_header")["block_header"]["hash"]==b_rpc("get_last_block_header")["block_header"]["hash"])
    def start_b():
        global b_process
        args=command(b_directory)
        for flag,value in (("--rpc-bind-port",str(b_rpc_port)),("--p2p-bind-port",str(b_p2p)),("--add-exclusive-node",f"127.0.0.1:{p2p_port}"),("--log-file",str(scratch/"daemon-b.log"))):args[args.index(flag)+1]=value
        b_process=subprocess.Popen(args,stdin=subprocess.DEVNULL,stdout=b_log,stderr=subprocess.STDOUT)
        wait("node B RPC startup",lambda:b_rpc("get_info").get("height",0)>0)
    def stop_b():
        global b_process
        if b_process is None:return
        if b_process.poll() is None:
            try:b_request("/stop_daemon",{},10)
            except OSError:pass
            try:b_process.wait(timeout=30)
            except subprocess.TimeoutExpired:b_process.terminate();b_process.wait(timeout=15)
        b_process=None
    def mine_b(count,nonce=0):return b_rpc("generateblocks",{"amount_of_blocks":count,"reserve_size":20,"wallet_address":alice["address"],"prev_block":"","starting_nonce":nonce})
    def check_peers():
        wait("real loopback peer connections",lambda:bool(rpc("get_connections").get("connections")) and bool(b_rpc("get_connections").get("connections")))
        for get in (rpc,b_rpc):
            connections=get("get_connections")["connections"]
            check(all(row.get("ip")=="127.0.0.1" for row in connections),"all observed peer addresses are literal loopback")
    try:
        check(all(path.is_file() for path in (DAEMON,TOOL,BRIDGE,WALLET)),"native executables present")
        keys=scratch/"alice.keys";alice=bridge("create",keys)
        bobkeys=scratch/"bob.keys";bob=bridge("create",bobkeys)
        intruderkeys=scratch/"intruder.keys";bridge("create",intruderkeys)
        # Startup failures are tested before any P2P connection is allowed.
        for label,remove,replace,extra in [
            ("missing regtest","--regtest",{},[]),
            ("missing backing","--xgtx-backing-regtest",{},[]),
            ("missing persistent fixture","--keep-fakechain",{},[]),
            ("public network flag",None,{},["--testnet"]),
            ("wildcard P2P binding",None,{"--p2p-bind-ip":"0.0.0.0"},[]),
            ("wildcard RPC binding",None,{"--rpc-bind-ip":"0.0.0.0"},[]),
            ("external peer",None,{"--add-exclusive-node":"8.8.8.8:48080"},[]),
            ("DNS peer",None,{"--add-exclusive-node":"example.com:48080"},[])]:
            args=command(scratch/("reject-"+label.replace(" ","-")))
            if remove:args.remove(remove)
            for flag,value in replace.items():args[args.index(flag)+1]=value
            result=subprocess.run(args+extra,stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=30)
            check(result.returncode!=0 and "Loopback" in result.stdout+result.stderr or result.returncode!=0 and "regtest" in result.stdout+result.stderr,"startup refuses "+label)
        info=start();start_b();connected=True
        check(info.get("offline") is False,"node A runs real P2P in isolated loopback mode")
        check(b_rpc("get_info").get("offline") is False,"node B runs real P2P in isolated loopback mode")
        check_peers();wait_equal()
        print("Mining native funding on node A; node B must sync over P2P...",flush=True)
        mine(alice["address"],80);wait_equal()
        check(b_rpc("get_info")["height"]==81,"node B catches up all eighty native funding blocks")
        genesis=rpc("get_block",{"height":0})["block_header"]["hash"]
        funding=scratch/"funding.bin"
        with funding.open("wb") as out:
            out.write(b"XGTXFUND1"+bytes.fromhex(genesis)+struct.pack("<QQI",181,COIN,16))
            for height in range(1,17):
                block=rpc("get_block",{"height":height})
                transaction=request("/get_transactions",{"txs_hashes":[block["miner_tx_hash"]],"decode_as_json":False,"prune":False,"split":False})["txs"][0]
                blob=bytes.fromhex(block["blob"]);out.write(struct.pack("<QI",transaction["output_indices"][0],len(blob))+blob)
        alice_wallet_log=start_rpc_wallet(keys,"owner")
        spec=importlib.util.spec_from_file_location("xgtx_local_trader",ROOT/"xgtx-trader/server.py")
        trader_module=importlib.util.module_from_spec(spec);spec.loader.exec_module(trader_module)
        trader=trader_module.Trader(trader_module.RPC(url),trader_module.RPC(f"http://127.0.0.1:{wallet_port}"))
        interface=trader_module.make_server(trader,0);interface_thread=threading.Thread(target=interface.serve_forever,daemon=True);interface_thread.start()
        ui("refresh")
        deposit=bridge("deposit",keys,funding,0);state=confirm(deposit,"P2P backed deposit")
        check(state["native_locked"]==49*COIN,"peer-backed reserve credits exact native amount")
        _,state=act(1,2,"P2P ALPHA issuance",ticker="ALPHA",quantity=1_000_000)
        alpha=next(item["id"] for item in state["assets"] if item["ticker"]=="ALPHA")
        _,state=act(4,3,"P2P native/token pool",asset=ZERO,other=alpha,quantity=20*COIN,limit=500_000)
        _,state=act(7,4,"P2P native/token swap",asset=ZERO,other=alpha,quantity=10*COIN,minimum=1)
        check(balance(state,TREASURY,ZERO)==10*COIN*5//10000,"both peers conserve actual protocol fee")
        shares=state["pools"][0]["shares"]
        _,state=act(6,5,"P2P liquidity withdrawal",asset=ZERO,other=alpha,quantity=shares,limit=1,minimum=1)
        mine_b(11);wait_equal()
        _,release=withdraw(6,10*COIN,"P2P native withdrawal")
        state=confirm(release,"P2P native withdrawal")
        common=economic(state);common_tip=state["tip"]
        # Partition by restarting only these child nodes offline. Same linked
        # databases/consensus; no block blobs are injected through RPC.
        connected=False;stop();stop_b();partitioned=True;start();start_b()
        check(rpc("get_info")["offline"] is True and b_rpc("get_info")["offline"] is True,"explicit partition has no peer sockets")
        _,orphan=withdraw(7,10*COIN,"losing-branch native withdrawal")
        losing=economic(confirm(orphan,"node A shorter branch withdrawal"))
        check(losing["native_locked"]==29*COIN,"shorter branch spends protected reserves")
        mine_b(3,9000)
        winning_tip=b_rpc("get_last_block_header")["block_header"]["hash"]
        check(winning_tip!=common_tip and rpc("get_info")["height"]<b_rpc("get_info")["height"],"independent node B branch has greater cumulative work at fixed difficulty")
        stop();stop_b();partitioned=False;start();start_b();connected=True;check_peers();wait_equal()
        restored=audited("connected longer-branch reorganization")
        check(restored["tip"]==winning_tip,"node A adopts independently mined node B branch over P2P")
        check(economic(restored)==common,"connected reorg restores exact reserve backing, balances, treasury and owner nonce")
        # Orphans may be reinserted into a pool by normal reorg handling. Clear
        # only these fixture pools, then prove original input/nonce replay.
        rpc("flush_txpool");b_rpc("flush_txpool")
        replayed=confirm(orphan,"native reserve withdrawal replay after connected reorg")
        check(economic(replayed)==losing,"replayed withdrawal restores identical economic state on both peers")
        stop_b();start_b();check_peers();wait_equal()
        check(bridge("inspect",b_dbpath)==bridge("inspect",dbpath),"node B restart preserves identical complete native-backed exchange projection")
        wallet_rpc("stop_wallet");wallet_process.wait(timeout=30);wallet_process=None;alice_wallet_log.close()
        stop();stop_b()
        check(bridge("inspect",b_dbpath)==bridge("inspect",dbpath),"both stopped native databases agree read-only")
        print(f"{checks} checks passed: isolated loopback P2P sync, funded transaction relay, peer mining, native/token fees, protected withdrawal, independent longer branch, reorg rollback/replay and restart",flush=True)
        print("Localhost peers only. No external-network interoperability, public activation, production bootstrap or release hardening claimed.",flush=True)
    finally:
        if interface is not None:interface.shutdown();interface.server_close();interface_thread.join(timeout=5)
        if wallet_process is not None and wallet_process.poll() is None:
            wallet_process.terminate();wallet_process.wait(timeout=30)
        stop();stop_b();b_log.close()
