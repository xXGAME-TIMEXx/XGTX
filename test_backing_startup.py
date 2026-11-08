#!/usr/bin/env python3
"""Launch only a child offline regtest daemon in a fresh temporary directory."""
import json
import socket
import subprocess
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT=Path.home()/"xgtx-exchange-dev"
BUILD=ROOT/"build/exchange-integration"
DAEMON=BUILD/"bin/xgtxd"
INSPECT=BUILD/"tests/xgtx_node_smoke/xgtx_projection_inspect"
ADDRESS="CmS3B2SaPyCfwGAB7bsaqm6jT4qd5MHkPByjW5Db5jECQ7YkKw9A69rjAe8LZaXfHjQuyrybaKTa5S9ctmXnFQ3H7Y3ETjF"
LOG=ROOT/"xgtx-backing-startup-run.log"
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

with tempfile.TemporaryDirectory(prefix="xgtx-backing-node-") as scratch, LOG.open("w") as log:
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
    def aligned(label,height=None):
        info=rpc("get_info")
        header=rpc("get_last_block_header")["block_header"]
        state=projection()
        check(state["height"]==info["height"],label+": block counts match")
        check(state["tip"]==header["hash"],label+": tips match")
        check(state["assets"]==0 and state["balances"]==0,label+": no fabricated exchange credits")
        if height is not None:check(info["height"]==height,label+": expected height")
        return state
    try:
        check(DAEMON.is_file() and INSPECT.is_file(),"native executables exist")
        for missing in ("--regtest","--offline","--keep-fakechain"):
            args=command(scratch/("reject-"+missing[2:]))
            args.remove(missing)
            result=subprocess.run(args,capture_output=True,text=True,timeout=45)
            log.write(result.stdout+result.stderr);log.flush()
            check(result.returncode!=0 and "requires --regtest --offline --keep-fakechain" in result.stdout+result.stderr,
                  "startup rejects missing "+missing)
        args=command(scratch/"reject-no-exchange")
        args.remove("--xgtx-exchange-regtest")
        result=subprocess.run(args,capture_output=True,text=True,timeout=45)
        check(result.returncode!=0 and "requires --xgtx-exchange-regtest" in result.stdout+result.stderr,
              "backing startup requires exchange configuration")
        for network in ("--testnet","--stagenet"):
            args=command(scratch/("reject-"+network[2:]))+[network]
            result=subprocess.run(args,capture_output=True,text=True,timeout=45)
            check(result.returncode!=0,"backing startup rejects "+network)
        args=command(scratch/"reject-default-dir")
        directory_index=args.index("--data-dir");del args[directory_index:directory_index+2]
        result=subprocess.run(args,capture_output=True,text=True,timeout=45)
        check(result.returncode!=0 and "requires --regtest --offline --keep-fakechain" in result.stdout+result.stderr,
              "backing startup requires explicit data directory")
        info=start()
        check(info.get("offline") is True,"node runs offline")
        check(info.get("incoming_connections_count",0)==0 and info.get("outgoing_connections_count",0)==0,"no peer connections")
        check(dbpath.is_dir(),"projection uses isolated exchange-backed-v3 database")
        aligned("genesis",1)
        rpc("generateblocks",{"amount_of_blocks":3,"reserve_size":20,"wallet_address":ADDRESS,"prev_block":"","starting_nonce":0},120)
        aligned("three generated regression blocks",4)
        result=request("/pop_blocks",{"nblocks":1})
        check(result.get("status")=="OK","native block removal RPC succeeds")
        anchor=aligned("after native block removal",3)
        stop()
        start()
        restarted=aligned("after daemon restart",3)
        check(restarted==anchor,"projection persists across daemon restart")
        rpc("generateblocks",{"amount_of_blocks":2,"reserve_size":20,"wallet_address":ADDRESS,"prev_block":"","starting_nonce":100},120)
        aligned("continued mining after restart",5)
        result=request("/pop_blocks",{"nblocks":2})
        check(result.get("status")=="OK","persisted rollback succeeds")
        restored=aligned("restored earlier tip",3)
        check(restored==anchor,"native and exchange undo restore original tip")
        stop()
        check(projection()==anchor,"read-only inspection after shutdown")
        print(f"{checks} checks passed: offline node startup guards, generated native blocks, projection alignment, pop, restart, persisted undo",flush=True)
        print("Regression-test coins only; no funded backing transaction exercised. No token relay, funded RingCT carrier, native XGTX backing or mainnet activation tested.",flush=True)
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill();process.wait(timeout=10)
