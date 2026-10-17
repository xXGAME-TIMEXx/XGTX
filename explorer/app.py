"""XGTX public explorer. Run with gunicorn on loopback behind nginx."""
import re
import time
from datetime import datetime, timezone
from decimal import Decimal
from html import escape
from urllib.parse import urlencode

import requests
from flask import Flask, abort, redirect, request

app = Flask(__name__)
app.config['MAX_CONTENT_LENGTH'] = 1024
RPC = 'http://127.0.0.1:48081'
GENESIS = '6cc86a50eca6febe795dc25490d02f29fe8a5590e93daf03d1e0c368191d7435'
HASH = re.compile(r'[0-9a-fA-F]{64}\Z')
_identity_until = 0


def call(path, payload=None):
    with requests.Session() as session:
        session.trust_env = False
        if payload is None:
            response = session.get(RPC + path, timeout=(2, 8))
        else:
            response = session.post(RPC + path, json=payload, timeout=(2, 8))
        response.raise_for_status()
        if len(response.content) > 8_000_000:
            abort(502, 'Node response too large.')
        data = response.json()
    if data.get('error'):
        abort(404, 'Block not found on this node.')
    result = data.get('result', data)
    if result.get('status') != 'OK':
        abort(503, 'Node is not ready.')
    return result


def rpc(method, params):
    return call('/json_rpc', {'jsonrpc': '2.0', 'id': 'explorer',
                              'method': method, 'params': params})


def info():
    global _identity_until
    data = call('/get_info')
    if not data.get('mainnet') or data.get('testnet') or data.get('stagenet'):
        abort(503, 'Node network identity mismatch.')
    if time.monotonic() >= _identity_until:
        genesis = rpc('get_block_header_by_height', {'height': 0})
        if genesis.get('block_header', {}).get('hash') != GENESIS:
            abort(503, 'Node genesis identity mismatch.')
        _identity_until = time.monotonic() + 30
    return data


def e(value):
    return escape(str(value), quote=True)


def coins(value):
    return format(Decimal(int(value)) / Decimal(10**10), '.10f') + ' XGTX'


def stamp(value):
    return datetime.fromtimestamp(int(value), timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')


def link(kind, value):
    return '<a href="/' + kind + '/' + e(value) + '">' + e(value) + '</a>'


def page(title, body, status=200):
    return ('<!doctype html><html lang="en"><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1">'
            '<title>' + e(title) + ' | XGTX Explorer</title><style>'
            'body{background:#111827;color:#e5e7eb;font:16px system-ui;max-width:1100px;margin:32px auto;padding:0 20px}'
            'a{color:#6ee7b7}input,button{font:inherit;padding:10px;margin:4px 0;max-width:100%;box-sizing:border-box}'
            'input{width:75%}table{border-collapse:collapse;width:100%}td,th{padding:12px 8px;text-align:left;border-bottom:1px solid #374151}'
            'td{overflow-wrap:anywhere}main{overflow-x:auto}code{overflow-wrap:anywhere}.note{color:#a7b0c2}li{margin:12px 0}'
            '</style><header><h1><a href="/">XGTX Explorer</a></h1><p>Public beta · Native mainnet</p>'
            '<form action="/search" method="get"><input name="q" maxlength="64" required aria-label="Block height, block hash or transaction hash" '
            'placeholder="Block height, block hash or transaction hash"><button>Search</button></form></header>'
            '<main><h2>' + e(title) + '</h2>' + body + '</main><footer><p class="note">'
            'Data reflects this node’s view of the chain. Confirmation counts can change during reorganizations. '
            'This explorer does not reveal hidden transfer amounts, recipients or wallet balances.</p>'
            '<a href="https://github.com/xXGAME-TIMEXx/XGTX">Downloads and project information</a></footer></html>', status)


@app.after_request
def headers(response):
    response.headers['Content-Security-Policy'] = "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'"
    response.headers['X-Content-Type-Options'] = 'nosniff'
    response.headers['Referrer-Policy'] = 'no-referrer'
    response.headers['Cache-Control'] = 'no-store'
    return response


@app.errorhandler(requests.RequestException)
@app.errorhandler(ValueError)
@app.errorhandler(KeyError)
@app.errorhandler(TypeError)
def unavailable(error):
    return page('Node unavailable', '<p>Unable to read chain data. Please try again shortly.</p>', 502)


from werkzeug.exceptions import HTTPException


@app.errorhandler(HTTPException)
def http_error(error):
    return page(error.name, '<p>' + e(error.description) + '</p>', error.code)


@app.get('/')
def home():
    data = info()
    top = int(data['height']) - 1
    end_text = request.args.get('end', str(top))
    if not re.fullmatch(r'[0-9]{1,12}', end_text):
        abort(400, 'Invalid page height.')
    end = min(int(end_text), top)
    if end < 0:
        abort(503, 'No blocks available.')
    start = max(0, end - 19)
    blocks = rpc('get_block_headers_range', {'start_height': start, 'end_height': end})['headers']
    rows = ''.join('<tr><td>' + link('block', b['height']) + '</td><td>' + e(stamp(b['timestamp'])) +
                   '</td><td>' + e(b['num_txes']) + '</td><td>' + e(coins(b['reward'])) + '</td></tr>'
                   for b in sorted(blocks, key=lambda b: int(b['height']), reverse=True))
    body = '<p>Block count: <b>' + e(data['height']) + '</b> · Tip index: <b>' + e(top) + '</b> · Node reports synchronized: <b>' + e(data.get('synchronized')) + '</b></p>'
    body += '<p>Pending transactions: ' + e(data['tx_pool_size']) + '</p><table><tr><th>Block index</th><th>Time</th><th>Transactions*</th><th>Miner reward + fees</th></tr>' + rows + '</table><p>*Excludes the miner transaction.</p>'
    if start:
        body += '<p><a href="/?' + urlencode({'end': start - 1}) + '">Older blocks</a></p>'
    return page('Recent blocks', body)


@app.get('/search')
def search():
    query = request.args.get('q', '').strip()
    if re.fullmatch(r'[0-9]{1,12}', query):
        return redirect('/block/' + str(int(query)))
    if HASH.fullmatch(query):
        return page('Choose lookup', '<p>' + link('block', query.lower()) + ' (block)</p><p>' + link('tx', query.lower()) + ' (transaction)</p>')
    abort(400, 'Enter a block index or a 64-character hexadecimal hash.')


@app.get('/block/<query>')
def block(query):
    if re.fullmatch(r'[0-9]{1,12}', query):
        params = {'height': int(query)}
    elif HASH.fullmatch(query):
        params = {'hash': query.lower()}
    else:
        abort(400, 'Invalid block identifier.')
    info()
    data = rpc('get_block', params)
    b = data['block_header']
    fields = [('Index', b['height']), ('Hash', b['hash']), ('Previous hash', b['prev_hash']),
              ('Time', stamp(b['timestamp'])), ('Protocol version', b['major_version']),
              ('Orphan status', b['orphan_status']), ('Blocks after this block', b['depth']),
              ('Size in bytes', b['block_size']), ('Difficulty', b.get('wide_difficulty', b.get('difficulty'))),
              ('Miner reward including fees', coins(b['reward']))]
    body = '<table>' + ''.join('<tr><th>' + e(k) + '</th><td>' + e(v) + '</td></tr>' for k, v in fields) + '</table>'
    body += '<h3>Miner transaction</h3>' + link('tx', data['miner_tx_hash'])
    txs = data.get('tx_hashes', [])
    body += '<h3>Transactions (' + e(len(txs)) + ')</h3><ul>' + ''.join('<li>' + link('tx', tx) + '</li>' for tx in txs[:500]) + '</ul>'
    if len(txs) > 500:
        body += '<p>Only the first 500 transaction links are displayed.</p>'
    return page('Block ' + str(b['height']), body)


@app.get('/tx/<txid>')
def transaction(txid):
    if not HASH.fullmatch(txid):
        abort(400, 'Invalid transaction hash.')
    info()
    data = call('/get_transactions', {'txs_hashes': [txid.lower()], 'decode_as_json': False, 'prune': True})
    if not data.get('txs'):
        abort(404, 'Transaction not found on this node.')
    tx = data['txs'][0]
    body = '<p>Hash: <code>' + e(tx['tx_hash']) + '</code></p>'
    if tx['in_pool']:
        body += '<p><b>Pending in this node’s transaction pool.</b> Not yet confirmed in a block.</p>'
    else:
        body += '<p>Included in block ' + link('block', tx['block_height']) + '</p><p>Confirmations reported by node: ' + e(tx['confirmations']) + '</p><p>Block time: ' + e(stamp(tx['block_timestamp'])) + '</p>'
    return page('Transaction', body)
