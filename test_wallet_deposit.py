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
LOG=ROOT/"xgtx-wallet-deposit-run.log"
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
        wallet_dir=scratch/(label+"-wallets");wallet_dir.mkdir(mode=0o700)
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
        restore=json.loads(params.read_text());wallet_rpc("generate_from_keys",restore);del restore;params.unlink()
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
        if tx.get("already_relayed"):check(tx["hash"] in pool(),label+": wallet-relayed native admission")
        else:
            response=submit(tx);check(response.get("status")=="OK",label+": native mempool admission: "+str(response))
        check(tx["hash"] in pool(),label+": stored in native mempool")
        height=rpc("get_info")["height"];mine(alice["address"],1,height*100)
        check(tx["hash"] in rpc("get_block",{"height":height})["tx_hashes"],label+": normal template/mining")
        return audited(label)
    def act(kind,nonce,label,**fields):
        wallet_rpc("refresh")
        payload={"kind":kind,"quantity":"1","max_fee":str(COIN),"do_not_relay":nonce!=5,**fields}
        for field in ("quantity","limit","minimum"): 
            if field in payload:payload[field]=str(payload[field])
        preview=economic(bridge("inspect",dbpath))
        result=wallet_rpc("xgtx_operation",payload)
        check(result["owner"]==alice["owner"] and result["chain"]==genesis and result["nonce"]==str(nonce),label+": wallet binds owner, chain and next nonce")
        check(0<int(result["fee"])<=COIN and int(result["weight"])>0,label+": wallet estimates bounded native fee")
        tx={"hash":result["tx_hash"],"tx_as_hex":result["tx_blob"],"already_relayed":result["relayed"],"asset_id":result["asset_id"],"wallet_fee":result["fee"]}
        check(economic(bridge("inspect",dbpath))==preview,label+": wallet signing never changes confirmed balances")
        if payload["do_not_relay"]:check(tx["hash"] not in pool(),label+": preview does not relay")
        else:check(tx["hash"] in pool(),label+": wallet RPC relays operation")
        state=confirm(tx,label)
        if kind==1:check(result["asset_id"]==next(a["id"] for a in state["assets"] if a["ticker"]==fields["ticker"]),"wallet returns actual confirmed token ID")
        return tx,state
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
        funding=scratch/"funding.bin"
        with funding.open("wb") as out:
            out.write(b"XGTXFUND1"+bytes.fromhex(genesis)+struct.pack("<QQI",181,COIN,16))
            for height in range(1,17):
                block=rpc("get_block",{"height":height})
                txs=request("/get_transactions",{"txs_hashes":[block["miner_tx_hash"]],"decode_as_json":False,"prune":False,"split":False})["txs"]
                check(len(txs)==1 and len(txs[0]["output_indices"])==1,"native funding output "+str(height))
                blob=bytes.fromhex(block["blob"]);out.write(struct.pack("<QI",txs[0]["output_indices"][0],len(blob))+blob)
        alice_wallet_log=start_rpc_wallet(keys,"owner")
        wallet_rpc("refresh")
        deposit=bridge("deposit",keys,funding,0)
        deposited=confirm(deposit,"chain-funded XGTX deposit")
        check(balance(deposited,alice["owner"],ZERO)==deposit["debit"]==49*COIN,"exact actual native deposit credited")
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
        wallet_deposit,state=act(8,2,"exact wallet XGTX deposit",quantity=5*COIN,limit=1)
        check(state["native_locked"]==54*COIN,"wallet deposit creates exact backed native supply")
        check(balance(state,alice["owner"],ZERO)==54*COIN,"wallet deposit credits the authenticated owner")
        check(rpc("xgtx_get_exchange",{"owner":alice["owner"],"limit":1})["reserve_outputs"]=="17","wallet deposit adds one segregated reserve record")
        wallet_rpc("refresh")
        wallet_after=int(wallet_rpc("get_balance")["balance"])
        reward=rpc("get_block",{"height":state["height"]-1})["block_header"]["reward"]
        check(wallet_before+reward-wallet_after==5*COIN+int(wallet_deposit["wallet_fee"]),"wallet native balance pays exact deposit plus mining fee")
        _,state=act(1,3,"ALPHA issuance",ticker="ALPHA",quantity=1_000_000)
        alpha=next(a["id"] for a in state["assets"] if a["ticker"]=="ALPHA")
        _,state=act(4,4,"XGTX/ALPHA pool",asset=ZERO,other=alpha,quantity=20*COIN,limit=500_000)
        check(reserves(state,ZERO)==20*COIN and reserves(state,alpha)==500_000,"native/token reserves backed on chain")
        before_quote=economic(state)
        maximum=(1<<64)-1
        boundary=rpc("xgtx_quote_swap",{"asset_in":ZERO,"asset_out":alpha,"amount_in":str(maximum)})
        effective=maximum-maximum*25//10000-maximum*5//10000
        check(boundary["amount_out"]==str(500_000*effective//(20*COIN+effective)),"maximum uint64 quote has exact output")
        check(boundary["lp_fee"]==str(maximum*25//10000) and boundary["protocol_fee"]==str(maximum*5//10000),"maximum uint64 quote has exact fees")
        quoted=rpc("xgtx_quote_swap",{"asset_in":ZERO,"asset_out":alpha,"amount_in":str(10*COIN)})
        check(quoted["tip"]==state["tip"] and quoted["height"]==state["height"],"quote binds confirmed snapshot")
        check(quoted["lp_fee"]==str(10*COIN*25//10000) and quoted["protocol_fee"]==str(10*COIN*5//10000),"quote uses exact disclosed fees")
        reverse=rpc("xgtx_quote_swap",{"asset_in":alpha,"asset_out":ZERO,"amount_in":"10000"})
        check(int(reverse["amount_out"])>0 and reverse["pool"]==quoted["pool"],"reverse-direction quote uses same pool")
        check(economic(bridge("inspect",dbpath))==before_quote,"quote RPC does not mutate backing or balances")
        for method,payload in (("xgtx_get_exchange",{"limit":0}),("xgtx_get_exchange",{"limit":101}),("xgtx_get_exchange",{"owner":"invalid"}),("xgtx_get_exchange",{"after_asset":"bad"}),
                               ("xgtx_quote_swap",{"asset_in":ZERO,"asset_out":alpha,"amount_in":"0"}),("xgtx_quote_swap",{"asset_in":ZERO,"asset_out":alpha,"amount_in":"18446744073709551616"}),
                               ("xgtx_quote_swap",{"asset_in":ZERO,"asset_out":alpha,"amount_in":"01"}),("xgtx_quote_swap",{"asset_in":ZERO,"asset_out":ZERO,"amount_in":"1"})):
            reply=request("/json_rpc",{"jsonrpc":"2.0","id":"reject","method":method,"params":payload})
            check(reply.get("error",{}).get("code")==-1,"RPC rejects malformed amount, page or pair: "+method+" "+repr(payload)+" reply="+repr(reply))
        _,state=act(7,5,"XGTX to ALPHA swap",asset=ZERO,other=alpha,quantity=10*COIN,minimum=1)
        check(balance(state,alice["owner"],alpha)-500_000==int(quoted["amount_out"]),"RPC quote equals actual mined swap output")
        treasury_fee=10*COIN*5//10_000;lp_fee=10*COIN*25//10_000
        check(balance(state,TREASURY,ZERO)==treasury_fee,"exact 0.05 percent XGTX fee credited to designated treasury")
        check(reserves(state,ZERO)==30*COIN-treasury_fee,"0.25 percent LP fee retained in native reserves")
        expected_output=500_000*(10*COIN-treasury_fee-lp_fee)//(20*COIN+10*COIN-treasury_fee-lp_fee)
        check(balance(state,alice["owner"],alpha)==500_000+expected_output,"exact native/token swap output")
        old_shares=state["pools"][0]["shares"]
        _,state=act(5,6,"add native/token liquidity through wallet",asset=ZERO,other=alpha,quantity=COIN,limit=10_000,minimum=1)
        check(state["pools"][0]["shares"]>old_shares,"wallet liquidity addition increases LP shares")
        shares=state["pools"][0]["shares"]
        _,state=act(6,7,"remove native/token liquidity",asset=ZERO,other=alpha,quantity=shares,limit=1,minimum=1)
        check(not state["pools"] and not state["lp"],"full LP withdrawal removes pool and shares")
        check(balance(state,alice["owner"],ZERO)==54*COIN-treasury_fee,"LP withdrawal preserves owner and treasury claims")
        _,state=act(3,8,"burn token through wallet",asset=alpha,quantity=100)
        check(next(a["supply"] for a in state["assets"] if a["id"]==alpha)==999_900,"wallet burn reduces confirmed supply")
        _,state=act(2,9,"transfer token through wallet",asset=alpha,recipient=bob["address"],quantity=10_000)
        check(balance(state,bob["owner"],alpha)==10_000,"wallet token transfer credits recipient spend-public-key balance")
        mine(alice["address"],11,7000)
        before=economic(audited("mature reserves"))
        unauthorized=bridge("withdraw",intruderkeys,dbpath,genesis,bobkeys,1,10*COIN)
        response=submit(unauthorized);check(response.get("status")!="OK","daemon rejects public reserve signer without ledger balance")
        check(unauthorized["hash"] not in pool(),"unauthorized withdrawal absent from native pool")
        check(economic(audited("unauthorized attempt"))==before,"unauthorized withdrawal leaves backing untouched")
        withdrawal=bridge("withdraw",keys,dbpath,genesis,bobkeys,10,10*COIN)
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
        check(received["balance"]==9*COIN,"actual wallet RPC recognizes nine XGTX ordinary payout after native mining fee")
        mine(alice["address"],11,9000);wallet_rpc("refresh")
        received=wallet_rpc("get_balance")
        check(received["balance"]==9*COIN and received["unlocked_balance"]==9*COIN,"wallet payout matures to unlocked spendable balance")
        sent=wallet_rpc("transfer",{"destinations":[{"address":alice["address"],"amount":COIN}],"get_tx_hex":True})
        check(sent["tx_hash"] in pool(),"ordinary wallet can spend redeemed XGTX through native mempool")
        height=rpc("get_info")["height"];mine(alice["address"],1,10000)
        check(sent["tx_hash"] in rpc("get_block",{"height":height})["tx_hashes"],"ordinary wallet spend of redeemed XGTX mined")
        wallet_rpc("refresh");remaining=wallet_rpc("get_balance")
        check(remaining["balance"]==8*COIN-sent["fee"],"ordinary wallet spend conserves payout minus payment and native fee")
        check(economic(audited("ordinary wallet redemption spend"))==final,"ordinary wallet spend cannot recreate exchange credits")
        wallet_rpc("stop_wallet");wallet_process.wait(timeout=30);wallet_process=None;wallet_log.close()
        stop();check(economic(bridge("inspect",dbpath))==final,"final persisted economic state verified read-only")
        print(f"{checks} checks passed: exact wallet deposit plus fixture bootstrap, native/token pair, swap, treasury fee, LP removal, authorized native withdrawal, restart/rollback/replay and actual wallet balance",flush=True)
        print("Offline regression coins only. Native bridge wallet APIs, complete UI, multi-node operation, hardening and public activation remain unfinished.",flush=True)
    finally:
        if wallet_process is not None and wallet_process.poll() is None:
            wallet_process.terminate()
            try:wallet_process.wait(timeout=15)
            except subprocess.TimeoutExpired:wallet_process.kill();wallet_process.wait(timeout=10)
        if process is not None and process.poll() is None:
            process.terminate()
            try:process.wait(timeout=15)
            except subprocess.TimeoutExpired:process.kill();process.wait(timeout=10)
