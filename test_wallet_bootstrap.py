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
LOG=ROOT/"xgtx-wallet-bootstrap-run.log"
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
        return [str(DAEMON),"--xgtx-exchange-regtest","--xgtx-backing-regtest","--regtest","--offline","--keep-fakechain",
                "--data-dir",str(directory),"--fixed-difficulty","1","--non-interactive",
                "--rpc-bind-ip","127.0.0.1","--rpc-bind-port",str(rpc_port),"--rpc-ssl","disabled",
                "--p2p-bind-ip","127.0.0.1","--p2p-bind-port",str(p2p_port),"--no-zmq",
                "--disable-dns-checkpoints","--check-updates","disabled","--log-level","1",
                "--log-file",str(scratch/"daemon.log")]
    directory=scratch/"node"
    dbpath=directory/"fake/exchange-backed-v3/lmdb"
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
        height=rpc("get_info")["height"];mine(alice["address"],1,height*100)
        check(tx["hash"] in rpc("get_block",{"height":height})["tx_hashes"],label+": normal template/mining")
        tx["already_relayed"]=False
        return audited(label)
    def act(kind,nonce,label,**fields):
        wallet_rpc("refresh")
        payload={"kind":kind,"quantity":"1","max_fee":str(COIN),"do_not_relay":nonce!=6,**fields}
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
    try:
        check(all(p.is_file() for p in (DAEMON,TOOL,BRIDGE,WALLET)),"native daemon, builders and wallet RPC exist")
        keys=scratch/"alice.keys";alice=bridge("create",keys)
        bobkeys=scratch/"bob.keys";bob=bridge("create",bobkeys)
        intruderkeys=scratch/"intruder.keys";bridge("create",intruderkeys)
        check(all(k.stat().st_mode & 0o077==0 for k in (keys,bobkeys,intruderkeys)),"temporary fixture keys private")
        info=start();check(info.get("offline") is True,"backed lifecycle node offline")
        check(info.get("incoming_connections_count",0)==0 and info.get("outgoing_connections_count",0)==0,"no public peer connections")
        print("Mining 80 regression blocks for deposit and transaction fees...",flush=True);mine(alice["address"],80)
        genesis=rpc("get_block",{"height":0})["block_header"]["hash"]
        alice_wallet_log=start_rpc_wallet(keys,"owner")
        spec=importlib.util.spec_from_file_location("xgtx_local_trader",ROOT/"xgtx-trader/server.py")
        trader_module=importlib.util.module_from_spec(spec);spec.loader.exec_module(trader_module)
        trader=trader_module.Trader(trader_module.RPC(url),trader_module.RPC(f"http://127.0.0.1:{wallet_port}"))
        interface=trader_module.make_server(trader,0)
        interface_thread=threading.Thread(target=interface.serve_forever,daemon=True);interface_thread.start()
        identity=wallet_rpc("xgtx_identity")
        check(identity["owner"]==alice["owner"] and identity["address"]==alice["address"] and identity["chain"]==genesis,"public wallet identity API matches fixture, without keys")
        initial_ui=ui("refresh")
        check(initial_ui["identity"]==identity and initial_ui["node"]["offline"] is True,"authenticated interface reads actual offline node and wallet")
        check(isinstance(initial_ui["wallet"]["balance"],str),"interface preserves native balance as exact decimal string")
        wallet_rpc("refresh")
        check(rpc("xgtx_get_exchange",{"limit":1})["reserve_outputs"]=="0","bootstrap begins with no fixture reserve records")
        bootstrap_request={"kind":8,"bootstrap":True,"limit":"16","quantity":str(100*COIN),"max_fee":str(COIN),"do_not_relay":True}
        for bad in (dict(bootstrap_request,do_not_relay=False),dict(bootstrap_request,kind=1),dict(bootstrap_request,limit="1"),dict(bootstrap_request,quantity="16"),dict(bootstrap_request,max_fee="1")):
            try:wallet_rpc("xgtx_operation",bad)
            except RuntimeError:pass
            else:raise RuntimeError("Unsafe bootstrap request accepted")
            check(not pool(),"rejected bootstrap never relays")
        preview=wallet_rpc("xgtx_operation",bootstrap_request)
        bootstrap_quantity=int(preview["deposit_quantity"])
        check(bootstrap_quantity>=16 and bootstrap_quantity+int(preview["fee"])<=100*COIN,"reviewed bootstrap respects total spending cap and fee ceiling")
        check(not preview["relayed"] and not pool(),"bootstrap preview never relays")
        check(rpc("xgtx_get_exchange",{"limit":1})["reserve_outputs"]=="0","bootstrap preview does not write reserve state")
        exact=wallet_rpc("xgtx_relay_operation",{"draft_id":preview["draft_id"]})
        check(exact["tx_blob"]==preview["tx_blob"] and exact["tx_hash"]==preview["tx_hash"] and exact["deposit_quantity"]==preview["deposit_quantity"],"bootstrap relays exact reviewed bytes and credited amount")
        deposit={"hash":exact["tx_hash"],"tx_as_hex":exact["tx_blob"],"already_relayed":True,"debit":bootstrap_quantity}
        deposited=confirm(deposit,"ordinary wallet funded reserve bootstrap")
        check(balance(deposited,alice["owner"],ZERO)==bootstrap_quantity,"exact wallet-funded bootstrap credited")
        check(rpc("xgtx_get_exchange",{"limit":1})["reserve_outputs"]=="16","wallet bootstrap creates all sixteen protected outputs")
        try:wallet_rpc("xgtx_operation",bootstrap_request)
        except RuntimeError:pass
        else:raise RuntimeError("Bootstrap unexpectedly accepted a nonempty reserve")
        check(True,"bootstrap refuses an already initialized reserve")
        wallet_rpc("refresh")
        try:withdraw(2,bootstrap_quantity,"immature reserve rejection")
        except RuntimeError:pass
        else:raise RuntimeError("Wallet withdrawal accepted immature reserve ring")
        check(not pool(),"immature withdrawal does not enter pool")
        mine(alice["address"],11,6000)
        stable=rpc("xgtx_get_exchange",{"limit":1})
        ring=rpc("xgtx_get_reserve_ring",{"start":"0","tip":stable["tip"]})
        check(len(ring["outputs"])==16 and ring["count"]=="16" and ring["tip"]==stable["tip"],"bounded authenticated reserve ring RPC")
        for bad_start,bad_tip in (("-1",stable["tip"]),("01",stable["tip"]),("1",stable["tip"]),("18446744073709551616",stable["tip"]),("0","0"*64)):
            reply=request("/json_rpc",{"jsonrpc":"2.0","id":"reject","method":"xgtx_get_reserve_ring","params":{"start":bad_start,"tip":bad_tip}})
            check(reply.get("error",{}).get("code")==-1,"reserve RPC rejects malformed/out-of-range index or stale snapshot")
        full_preview,_=withdraw(2,bootstrap_quantity,"full reserve withdrawal preview")
        full_result,full_tx=withdraw(2,bootstrap_quantity,"full reserve withdrawal relay",True)
        check(struct.unpack_from("<Q",bytes.fromhex(full_result["signed_intent"]),194)[0]==0,"full withdrawal has no reserve change")
        emptied=confirm(full_tx,"full wallet withdrawal")
        check(emptied["native_locked"]==0 and balance(emptied,alice["owner"],ZERO)==0,"full wallet withdrawal releases all owner backing")
        full_payout=int(full_result["payout"])
        wallet_rpc("refresh")
        for bad in ({"kind":8,"quantity":"1","max_fee":str(COIN)},
                    {"kind":1,"quantity":"01","max_fee":str(COIN),"ticker":"BAD"},
                    {"kind":1,"quantity":"1000","max_fee":"1","ticker":"BAD"},
                    {"kind":1,"quantity":"1000","max_fee":str(COIN),"ticker":"BAD","deadline_window":0}):
            try:wallet_rpc("xgtx_operation",bad)
            except RuntimeError:pass
            else:raise RuntimeError("Invalid wallet operation unexpectedly succeeded")
            check(not pool(),"rejected wallet operation never relays or consumes nonce")
        wallet_before=int(wallet_rpc("get_balance")["balance"])
        wallet_deposit,state=act(8,3,"exact wallet XGTX deposit",quantity=54*COIN,limit=1)
        check(state["native_locked"]==54*COIN,"wallet deposit creates exact backed native supply")
        check(balance(state,alice["owner"],ZERO)==54*COIN,"wallet deposit credits the authenticated owner")
        check(rpc("xgtx_get_exchange",{"owner":alice["owner"],"limit":1})["reserve_outputs"]=="17","wallet deposit adds one segregated reserve record")
        wallet_rpc("refresh")
        wallet_after=int(wallet_rpc("get_balance")["balance"])
        reward=rpc("get_block",{"height":state["height"]-1})["block_header"]["reward"]
        check(wallet_before+reward-wallet_after==54*COIN+int(wallet_deposit["wallet_fee"]),"wallet native balance pays exact deposit plus mining fee")
        stale=ui_preview({"kind":1,"ticker":"STALE","quantity":"1000","max_fee":str(COIN)})
        _,state=act(1,4,"ALPHA issuance",ticker="ALPHA",quantity=1_000_000)
        try:ui("submit",{"draft_id":stale["draft_id"]})
        except RuntimeError:pass
        else:raise RuntimeError("Stale owner nonce draft relayed")
        check(not pool(),"native wallet draft relay rejects changed nonce without broadcasting")
        alpha=next(a["id"] for a in state["assets"] if a["ticker"]=="ALPHA")
        _,state=act(4,5,"XGTX/ALPHA pool",asset=ZERO,other=alpha,quantity=20*COIN,limit=500_000)
        check(reserves(state,ZERO)==20*COIN and reserves(state,alpha)==500_000,"native/token reserves backed on chain")
        before_quote=economic(state)
        maximum=(1<<64)-1
        boundary=ui("quote",{"asset_in":ZERO,"asset_out":alpha,"amount_in":str(maximum)})
        effective=maximum-maximum*25//10000-maximum*5//10000
        check(boundary["amount_out"]==str(500_000*effective//(20*COIN+effective)),"maximum uint64 quote has exact output")
        check(boundary["lp_fee"]==str(maximum*25//10000) and boundary["protocol_fee"]==str(maximum*5//10000),"maximum uint64 quote has exact fees")
        quoted=ui("quote",{"asset_in":ZERO,"asset_out":alpha,"amount_in":str(10*COIN)})
        check(quoted["tip"]==state["tip"] and quoted["height"]==state["height"],"quote binds confirmed snapshot")
        check(quoted["lp_fee"]==str(10*COIN*25//10000) and quoted["protocol_fee"]==str(10*COIN*5//10000),"quote uses exact disclosed fees")
        reverse=ui("quote",{"asset_in":alpha,"asset_out":ZERO,"amount_in":"10000"})
        check(int(reverse["amount_out"])>0 and reverse["pool"]==quoted["pool"],"reverse-direction quote uses same pool")
        check(economic(bridge("inspect",dbpath))==before_quote,"quote RPC does not mutate backing or balances")
        for method,payload in (("xgtx_get_exchange",{"limit":0}),("xgtx_get_exchange",{"limit":101}),("xgtx_get_exchange",{"owner":"invalid"}),("xgtx_get_exchange",{"after_asset":"bad"}),
                               ("xgtx_quote_swap",{"asset_in":ZERO,"asset_out":alpha,"amount_in":"0"}),("xgtx_quote_swap",{"asset_in":ZERO,"asset_out":alpha,"amount_in":"18446744073709551616"}),
                               ("xgtx_quote_swap",{"asset_in":ZERO,"asset_out":alpha,"amount_in":"01"}),("xgtx_quote_swap",{"asset_in":ZERO,"asset_out":ZERO,"amount_in":"1"})):
            reply=request("/json_rpc",{"jsonrpc":"2.0","id":"reject","method":method,"params":payload})
            check(reply.get("error",{}).get("code")==-1,"RPC rejects malformed amount, page or pair: "+method+" "+repr(payload)+" reply="+repr(reply))
        _,state=act(7,6,"XGTX to ALPHA swap",asset=ZERO,other=alpha,quantity=10*COIN,minimum=1)
        check(balance(state,alice["owner"],alpha)-500_000==int(quoted["amount_out"]),"RPC quote equals actual mined swap output")
        treasury_fee=10*COIN*5//10_000;lp_fee=10*COIN*25//10_000
        check(balance(state,TREASURY,ZERO)==treasury_fee,"exact 0.05 percent XGTX fee credited to designated treasury")
        check(reserves(state,ZERO)==30*COIN-treasury_fee,"0.25 percent LP fee retained in native reserves")
        expected_output=500_000*(10*COIN-treasury_fee-lp_fee)//(20*COIN+10*COIN-treasury_fee-lp_fee)
        check(balance(state,alice["owner"],alpha)==500_000+expected_output,"exact native/token swap output")
        old_shares=state["pools"][0]["shares"]
        _,state=act(5,7,"add native/token liquidity through wallet",asset=ZERO,other=alpha,quantity=COIN,limit=10_000,minimum=1)
        check(state["pools"][0]["shares"]>old_shares,"wallet liquidity addition increases LP shares")
        shares=state["pools"][0]["shares"]
        _,state=act(6,8,"remove native/token liquidity",asset=ZERO,other=alpha,quantity=shares,limit=1,minimum=1)
        check(not state["pools"] and not state["lp"],"full LP withdrawal removes pool and shares")
        check(balance(state,alice["owner"],ZERO)==54*COIN-treasury_fee,"LP withdrawal preserves owner and treasury claims")
        _,state=act(3,9,"burn token through wallet",asset=alpha,quantity=100)
        check(next(a["supply"] for a in state["assets"] if a["id"]==alpha)==999_900,"wallet burn reduces confirmed supply")
        _,state=act(2,10,"transfer token through wallet",asset=alpha,recipient=bob["address"],quantity=10_000)
        check(balance(state,bob["owner"],alpha)==10_000,"wallet token transfer credits recipient spend-public-key balance")
        mine(alice["address"],11,7000)
        before=economic(audited("mature reserves"))
        unauthorized=bridge("withdraw",intruderkeys,dbpath,genesis,bobkeys,1,10*COIN)
        response=submit(unauthorized);check(response.get("status")!="OK","daemon rejects public reserve signer without ledger balance")
        check(unauthorized["hash"] not in pool(),"unauthorized withdrawal absent from native pool")
        check(economic(audited("unauthorized attempt"))==before,"unauthorized withdrawal leaves backing untouched")
        for invalid in ({"quantity":"0"},{"max_fee":"1"},{"limit":"1"},{"recipient":"bad"}):
            payload={"kind":9,"quantity":str(10*COIN),"max_fee":str(COIN),"recipient":bob["address"],**invalid}
            try:wallet_rpc("xgtx_operation",payload)
            except RuntimeError:pass
            else:raise RuntimeError("Invalid wallet withdrawal accepted")
            check(not pool(),"rejected withdrawal cannot relay or consume nonce")
        partial_result,withdrawal=withdraw(11,10*COIN,"partial wallet withdrawal preview")
        check(struct.unpack_from("<Q",bytes.fromhex(partial_result["signed_intent"]),194)[0]>0,"partial withdrawal authenticates positive reserve change")
        expected_payout=full_payout+int(partial_result["payout"])
        state=confirm(withdrawal,"XGTX withdrawal to wallet")
        check(state["native_locked"]==44*COIN,"native withdrawal releases exact reserve debit including mining fee")
        check(balance(state,alice["owner"],ZERO)==44*COIN-treasury_fee,"withdrawal debits only authorized owner claim")
        check(balance(state,TREASURY,ZERO)==treasury_fee,"withdrawal preserves treasury earnings")
        final=economic(state);stop();start()
        check(economic(audited("daemon restart"))==final,"backed trading and withdrawal persist across restart")
        response=request("/pop_blocks",{"nblocks":1,"keep_txs":False});check(response.get("status")=="OK","withdrawal block rollback")
        check(economic(audited("withdrawal rollback"))==before,"rollback restores exact reserve and owner liabilities")
        state=confirm(withdrawal,"withdrawal replay")
        check(economic(state)==final,"deterministic backed withdrawal replay")
        wallet_rpc("stop_wallet");wallet_process.wait(timeout=30);wallet_process=None;alice_wallet_log.close()
        # Restore only the new temporary recipient account, never a user wallet.
        wallet_log=start_rpc_wallet(bobkeys,"recipient")
        wallet_rpc("refresh")
        received=wallet_rpc("get_balance")
        check(received["balance"]==expected_payout,"actual wallet RPC recognizes both wallet-built payouts after estimated native fees")
        mine(alice["address"],11,9000);wallet_rpc("refresh")
        received=wallet_rpc("get_balance")
        check(received["balance"]==expected_payout and received["unlocked_balance"]==expected_payout,"wallet payout matures to unlocked spendable balance")
        sent=wallet_rpc("transfer",{"destinations":[{"address":alice["address"],"amount":COIN}],"get_tx_hex":True})
        check(sent["tx_hash"] in pool(),"ordinary wallet can spend redeemed XGTX through native mempool")
        height=rpc("get_info")["height"];mine(alice["address"],1,10000)
        check(sent["tx_hash"] in rpc("get_block",{"height":height})["tx_hashes"],"ordinary wallet spend of redeemed XGTX mined")
        wallet_rpc("refresh");remaining=wallet_rpc("get_balance")
        check(remaining["balance"]==expected_payout-COIN-sent["fee"],"ordinary wallet spend conserves payout minus payment and native fee")
        check(economic(audited("ordinary wallet redemption spend"))==final,"ordinary wallet spend cannot recreate exchange credits")
        try:wallet_rpc("xgtx_operation",{"kind":9,"quantity":str(COIN),"max_fee":str(COIN),"recipient":alice["address"]})
        except RuntimeError:pass
        else:raise RuntimeError("Recipient without native ledger balance can withdraw reserves")
        check(not pool(),"wallet without ledger claims cannot spend public reserves")
        wallet_rpc("stop_wallet");wallet_process.wait(timeout=30);wallet_process=None;wallet_log.close()
        if os.environ.get("XGTX_TRADER_DEMO")=="1":
            alice_wallet_log=start_rpc_wallet(keys,"owner")
            with trader.lock:trader.wallet=trader_module.RPC(f"http://127.0.0.1:{wallet_port}")
            ui("refresh")
            if rpc("get_info").get("offline") is not True:raise RuntimeError("Interactive fixture must remain offline")
            def demo_miner():
                while not demo_stop.wait(5):
                    try:mine(alice["address"],1)
                    except Exception:demo_stop.set()
            demo_thread=threading.Thread(target=demo_miner,daemon=True);demo_thread.start()
            print(f"LOCAL TEST-COIN INTERFACE: http://127.0.0.1:{interface.server_port}/#token={interface.token}",flush=True)
            print("Fresh temporary wallet only; offline regression blocks are generated every five seconds. Not public trading.",flush=True)
            try:input("Press Enter here to stop the demo and delete temporary fixture wallets: ")
            finally:demo_stop.set();demo_thread.join(timeout=30)
            wallet_rpc("stop_wallet");wallet_process.wait(timeout=30);wallet_process=None;alice_wallet_log.close()
            stop()
        else:
            stop();check(economic(bridge("inspect",dbpath))==final,"final persisted economic state verified read-only")
        print(f"{checks} checks passed: ordinary wallet reserve bootstrap and exact deposit, native/token pair, swap, treasury fee, LP removal, exact reviewed transaction relay through authenticated local interface, all token/pool operations, full/partial wallet withdrawal, restart/rollback/replay and actual wallet balance",flush=True)
        print("Offline regression coins only. Wallet bridge integration tested offline. Offline bootstrap wallet path tested. Production activation, deployment validation, broader resource hardening and release packaging remain unfinished.",flush=True)
    finally:
        demo_stop.set()
        if interface is not None:interface.shutdown();interface.server_close();interface_thread.join(timeout=5)
        if wallet_process is not None and wallet_process.poll() is None:
            wallet_process.terminate()
            try:wallet_process.wait(timeout=15)
            except subprocess.TimeoutExpired:wallet_process.kill();wallet_process.wait(timeout=10)
        if process is not None and process.poll() is None:
            process.terminate()
            try:process.wait(timeout=15)
            except subprocess.TimeoutExpired:process.kill();process.wait(timeout=10)
