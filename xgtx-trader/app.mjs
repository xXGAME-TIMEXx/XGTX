import {parseUnits, formatUnits, integer, minimumOutput} from './amounts.mjs';

const $ = (id) => document.getElementById(id);
const ZERO = '0'.repeat(64);
const fragment = new URLSearchParams(location.hash.slice(1));
let token = fragment.get('token') || sessionStorage.getItem('xgtx-session') || '';
if (fragment.has('token')) { sessionStorage.setItem('xgtx-session', token); history.replaceState(null, '', '/'); }
let current, quote, draft, busy = false;
const assets = new Map(), pools = new Map();
function notice(message, error = false) { $('notice').textContent = message; $('notice').classList.toggle('error', error); }
async function api(route, data = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 150000);
  try {
    const response = await fetch('/api/' + route, {method: 'POST', headers: {'Content-Type': 'application/json', Authorization: 'Bearer ' + token}, body: JSON.stringify(data), signal: controller.signal});
    const result = await response.json();
    if (!response.ok) throw Error(result.error || 'Local request failed.');
    return result;
  } catch (error) {
    if (error.name === 'AbortError') throw Error('Local request timed out. Check the transaction hash before retrying a submission.');
    throw error;
  } finally { clearTimeout(timer); }
}
async function execute(action) {
  if (busy) return;
  busy = true;
  const buttons = [...document.querySelectorAll('button')].map(button => [button, button.disabled]);
  for (const [button] of buttons) button.disabled = true;
  try { await action(); } catch (error) { notice(error.message, true); }
  finally { for (const [button, disabled] of buttons) button.disabled = disabled; busy = false; $('swap-preview').disabled = !quote; }
}
function asset(id) { const value = assets.get(id); if (!value) throw Error('Load metadata for the selected asset first.'); return value; }
function units(value, id) { return parseUnits(value, asset(id).decimals); }
function amount(value, id = ZERO) { return formatUnits(value, id === ZERO ? 10 : asset(id).decimals); }
function label(id) { return assets.get(id)?.ticker || id.slice(0, 12) + '…'; }
function feeLimit() { return parseUnits($('max-fee').value, 10); }
function formData(id) { return Object.fromEntries(new FormData($(id))); }
function option(value, text) { const element = document.createElement('option'); element.value = value; element.textContent = text; return element; }
function invalidateQuote() { quote = undefined; $('swap-preview').disabled = true; $('quote').textContent = 'Get a new quote for your current amount and limits.'; }
function updateModes() {
  const lp = $('lp-mode').value;
  $('pool-picker').hidden = lp === 'create'; $('create-pair').hidden = lp !== 'create'; $('lp-minimum-field').hidden = lp === 'create';
  $('lp-amount-label').textContent = lp === 'remove' ? 'LP shares to burn (whole base units)' : 'Amount A';
  $('lp-limit-label').textContent = lp === 'create' ? 'Amount B' : lp === 'add' ? 'Maximum amount B' : 'Minimum asset A returned';
  $('lp-minimum-label').textContent = lp === 'remove' ? 'Minimum asset B returned' : 'Minimum LP shares (whole base units)';
  const pool = pools.get($('pool-select').value);
  $('pool-detail').textContent = lp === 'create' ? 'Initial amounts set the starting ratio. LP shares represent pool reserves; returns are not guaranteed.' : pool ? `${label(pool.asset_a)} reserve: ${assets.has(pool.asset_a) ? amount(pool.reserve_a, pool.asset_a) : pool.reserve_a + ' base units'} · ${label(pool.asset_b)} reserve: ${assets.has(pool.asset_b) ? amount(pool.reserve_b, pool.asset_b) : pool.reserve_b + ' base units'} · Your shares: ${pool.owner_shares || '0'}. Confirmed values can change before mining.` : 'No existing pair on the loaded pages.';
  const mode = $('asset-mode').value;
  $('token-picker').hidden = mode === 'create'; $('token-create').hidden = mode !== 'create'; $('token-recipient').hidden = mode !== 'transfer';
  $('token-amount-label').textContent = mode === 'create' ? 'Initial token supply' : mode === 'burn' ? 'Amount to burn permanently' : 'Amount to transfer';
  const withdraw = $('bridge-mode').value === 'withdraw';
  $('bridge-recipient').hidden = !withdraw;
  $('bridge-amount-label').textContent = withdraw ? 'Total XGTX debit, including mining fee' : 'XGTX to credit to exchange';
  $('bridge-help').textContent = withdraw ? 'The mining fee comes out of this debit. Preview shows the exact ordinary wallet payout. A standard native recipient address is required.' : 'The exact deposit is credited to your exchange balance. The native wallet pays its mining fee separately. An existing sixteen-output reserve bootstrap is required.';
}
function render() {
  const view = current.exchange;
  $('address').textContent = current.identity.address;
  $('treasury').textContent = view.treasury;
  $('chain').textContent = 'CHAIN ' + view.chain;
  $('height').textContent = `BLOCK ${view.height} · NONCE ${view.next_nonce}`;
  $('network').textContent = current.node?.offline ? 'OFFLINE REGRESSION' : 'LOCAL NODE';
  $('wallet-balance').textContent = formatUnits(current.wallet?.unlocked_balance || '0', 10) + ' XGTX';
  $('dex-balance').textContent = formatUnits(assets.get(ZERO)?.balance || '0', 10) + ' XGTX';
  $('positions').textContent = [...pools.values()].filter(pool => BigInt(pool.owner_shares || '0') > 0n).length.toString();
  for (const select of document.querySelectorAll('.asset-select')) {
    const previous = select.value;
    select.replaceChildren(...[...assets.values()].filter(item => select.id !== 'token-select' || item.id !== ZERO).map(item => option(item.id, `${item.ticker} · ${item.id.slice(0, 8)}`)));
    if (assets.has(previous)) select.value = previous;
  }
  for (const id of ['swap-out', 'lp-b']) {
    const select = $(id), other = $(id === 'swap-out' ? 'swap-in' : 'lp-a');
    if (select.value === other.value && select.options.length > 1) select.selectedIndex = 1;
  }
  const previous = $('pool-select').value;
  $('pool-select').replaceChildren(...[...pools.values()].map(pool => option(pool.id, `${label(pool.asset_a)} / ${label(pool.asset_b)} · ${pool.id.slice(0, 8)}`)));
  if (pools.has(previous)) $('pool-select').value = previous;
  $('balances').replaceChildren(...[...assets.values()].map(item => {
    const row = document.createElement('tr');
    for (const text of [item.ticker, formatUnits(item.balance || '0', item.decimals), formatUnits(item.supply, item.decimals), item.id]) {
      const cell = document.createElement('td'); cell.textContent = text; cell.title = text; row.append(cell);
    }
    return row;
  }));
  $('more').hidden = !view.next_asset && !view.next_pool;
  updateModes();
}
async function loadState(refresh = false) {
  current = await api(refresh ? 'refresh' : 'state');
  assets.clear(); pools.clear();
  for (const item of current.exchange.assets) assets.set(item.id, item);
  for (const item of current.exchange.pools) pools.set(item.id, item);
  invalidateQuote(); render();
  notice(current.node.offline ? 'Connected to the offline regression exchange. These are test coins; public activation is not enabled.' : 'Connected to your local wallet. Confirm the chain and balances before trading.');
}
function reviewRow(name, value) {
  const term = document.createElement('dt'), description = document.createElement('dd');
  term.textContent = name; description.textContent = value; $('review-details').append(term, description);
}
async function preview(payload, title, details) {
  if (!current) throw Error('Connect your wallet first.');
  const result = await api('preview', {...payload, max_fee: feeLimit(), expected_owner: current.identity.owner, expected_chain: current.identity.chain});
  draft = result;
  $('review-title').textContent = title; $('review-details').replaceChildren();
  for (const [name, value] of details) reviewRow(name, value);
  if (payload.asset) reviewRow('Asset ID', payload.asset);
  if (payload.other) reviewRow('Paired asset ID', payload.other);
  reviewRow('Native mining fee', formatUnits(result.fee, 10) + ' XGTX');
  if (result.payout) reviewRow('Recipient payout', formatUnits(result.payout, 10) + ' XGTX');
  reviewRow('Owner nonce', result.nonce); reviewRow('Deadline block', result.deadline);
  reviewRow('Transaction hash', result.tx_hash);
  $('review').showModal(); notice('Preview ready. No transaction has been submitted.');
}

for (const button of document.querySelectorAll('[data-tab]')) button.addEventListener('click', () => {
  for (const tab of document.querySelectorAll('.tab-panel')) tab.hidden = tab.id !== button.dataset.tab;
  for (const item of document.querySelectorAll('[data-tab]')) item.classList.toggle('active', item === button);
});
$('refresh').addEventListener('click', () => execute(() => loadState(true)));
$('copy-address').addEventListener('click', () => execute(async () => { await navigator.clipboard.writeText(current.identity.address); notice('Full wallet address copied.'); }));
for (const id of ['lp-mode', 'pool-select', 'asset-mode', 'bridge-mode']) $(id).addEventListener('change', updateModes);
$('swap-form').addEventListener('input', invalidateQuote);
$('get-quote').addEventListener('click', () => execute(async () => {
  const data = formData('swap-form');
  const quantity = units(data.amount, data.asset);
  const bps = Number(parseUnits(data.slippage, 2, false));
  const result = await api('quote', {asset_in: data.asset, asset_out: data.other, amount_in: quantity});
  const minimum = minimumOutput(result.amount_out, bps);
  quote = {result, quantity, minimum, asset: data.asset, other: data.other};
  $('quote').textContent = `Estimated receive: ${amount(result.amount_out, data.other)} ${label(data.other)}. Minimum receive: ${amount(minimum, data.other)}. LP fee: ${amount(result.lp_fee, data.asset)} ${label(data.asset)}. Treasury fee: ${amount(result.protocol_fee, data.asset)} ${label(data.asset)}. Quote block: ${result.height}.`;
  notice('Quote ready. Preview the transaction to inspect the native mining fee.');
}));
$('swap-form').addEventListener('submit', event => { event.preventDefault(); execute(async () => {
  if (!quote) throw Error('Get a fresh quote first.');
  const q = quote;
  await preview({kind: 7, asset: q.asset, other: q.other, quantity: q.quantity, minimum: q.minimum}, 'Swap ' + label(q.asset) + ' → ' + label(q.other), [
    ['Pay', amount(q.quantity, q.asset) + ' ' + label(q.asset)], ['Receive at least', amount(q.minimum, q.other) + ' ' + label(q.other)],
    ['LP fee', amount(q.result.lp_fee, q.asset) + ' ' + label(q.asset)], ['Treasury fee', amount(q.result.protocol_fee, q.asset) + ' ' + label(q.asset)]]);
}); });
$('liquidity-form').addEventListener('submit', event => { event.preventDefault(); execute(async () => {
  const data = formData('liquidity-form'); let a = data.asset, b = data.other;
  if (data.mode !== 'create') { const pool = pools.get(data.pool); if (!pool) throw Error('Select a confirmed pool first.'); a = pool.asset_a; b = pool.asset_b; }
  if (a === b) throw Error('A pair requires two different assets.');
  const quantity = data.mode === 'remove' ? integer(data.amount, true).toString() : units(data.amount, a);
  const limit = units(data.limit, data.mode === 'remove' ? a : b);
  const minimum = data.mode === 'create' ? '0' : data.mode === 'remove' ? units(data.minimum, b) : integer(data.minimum, true).toString();
  await preview({kind: data.mode === 'create' ? 4 : data.mode === 'add' ? 5 : 6, asset: a, other: b, quantity, limit, minimum}, data.mode + ' liquidity', [
    ['Pair', label(a) + ' / ' + label(b)], [data.mode === 'remove' ? 'LP shares to burn' : 'Asset A', data.mode === 'remove' ? quantity : amount(quantity, a) + ' ' + label(a)],
    [data.mode === 'remove' ? 'Minimum A returned' : data.mode === 'add' ? 'Maximum B' : 'Asset B', amount(limit, data.mode === 'remove' ? a : b)],
    ...(data.mode === 'create' ? [] : [[data.mode === 'remove' ? 'Minimum B returned' : 'Minimum LP shares', data.mode === 'remove' ? amount(minimum, b) : minimum]])]);
}); });
$('asset-form').addEventListener('submit', event => { event.preventDefault(); execute(async () => {
  const data = formData('asset-form');
  if (data.mode === 'create') {
    if (!/^[A-Z]{2,12}$/.test(data.ticker) || data.ticker === 'XGTX') throw Error('Use an uppercase ticker, 2–12 letters, other than XGTX.');
    const decimals = Number(integer(data.decimals));
    if (decimals > 12) throw Error('Token decimals must be between 0 and 12.');
    const quantity = parseUnits(data.amount, decimals);
    await preview({kind: 1, ticker: data.ticker, decimals, quantity}, 'Create token', [['Ticker', data.ticker], ['Initial supply', formatUnits(quantity, decimals)], ['Decimals', String(decimals)]]);
  } else {
    if (data.asset === ZERO) throw Error('Use the native deposit/withdraw screen for XGTX.');
    const quantity = units(data.amount, data.asset);
    const payload = {kind: data.mode === 'burn' ? 3 : 2, asset: data.asset, quantity};
    if (data.mode === 'transfer') payload.recipient = data.recipient;
    await preview(payload, data.mode === 'burn' ? 'Burn tokens permanently' : 'Transfer tokens', [['Amount', amount(quantity, data.asset) + ' ' + label(data.asset)], ...(payload.recipient ? [['Recipient', payload.recipient]] : [])]);
  }
}); });
$('bridge-form').addEventListener('submit', event => { event.preventDefault(); execute(async () => {
  const data = formData('bridge-form'), quantity = parseUnits(data.amount, 10);
  const payload = data.mode === 'deposit' ? {kind: 8, quantity, limit: '1'} : {kind: 9, quantity, recipient: data.recipient};
  await preview(payload, data.mode === 'deposit' ? 'Deposit native XGTX' : 'Withdraw native XGTX', [
    [data.mode === 'deposit' ? 'Exchange credit' : 'Total ledger debit', formatUnits(quantity, 10) + ' XGTX'],
    ...(payload.recipient ? [['Recipient', payload.recipient], ['Fee treatment', 'Mining fee is included in the total debit.']] : [['Fee treatment', 'Native wallet pays mining fee separately.']])]);
}); });
$('more').addEventListener('click', () => execute(async () => {
  const view = current.exchange;
  const result = await api('state', {after_asset: view.next_asset || [...assets.keys()].at(-1) || '', after_pool: view.next_pool || [...pools.keys()].at(-1) || '', tip: view.tip});
  if (result.identity.owner !== current.identity.owner || result.identity.chain !== current.identity.chain) throw Error('Open wallet changed; refresh balances.');
  for (const item of result.exchange.assets) assets.set(item.id, item);
  for (const item of result.exchange.pools) pools.set(item.id, item);
  current.exchange = result.exchange; render();
}));
$('cancel-review').addEventListener('click', () => { draft = undefined; $('review').close(); });
$('review').addEventListener('cancel', () => { draft = undefined; });
$('submit-review').addEventListener('click', () => execute(async () => {
  if (!draft) throw Error('Preview a transaction first.');
  const reviewed = draft; draft = undefined;
  try {
    const result = await api('submit', {draft_id: reviewed.draft_id});
    $('review').close(); $('receipt').hidden = false; $('receipt').querySelector('h2').textContent = 'Transaction sent'; $('receipt-hash').textContent = result.tx_hash;
    notice('Exact reviewed transaction submitted. Refresh after a block is mined to confirm balances.');
    invalidateQuote();
  } catch (error) {
    $('review').close(); $('receipt').hidden = false; $('receipt-hash').textContent = reviewed.tx_hash;
    $('receipt').querySelector('h2').textContent = 'Submission needs checking';
    throw Error(error.message + ' Check the displayed hash before trying another transaction.');
  }
}));
if (!token) notice('Start the local launcher and open its session link. No wallet seed or private key is required in this interface.', true);
else execute(() => loadState());
