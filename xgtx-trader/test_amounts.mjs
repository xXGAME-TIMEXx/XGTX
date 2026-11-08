import test from 'node:test';
import assert from 'node:assert/strict';
import {MAX, parseUnits, formatUnits, minimumOutput} from './amounts.mjs';
test('uint64 precision survives display and entry beyond JavaScript Number range', () => {
  const raw = MAX.toString();
  assert.equal(formatUnits(raw, 10), '1844674407.3709551615');
  assert.equal(parseUnits(formatUnits(raw, 10), 10), raw);
  assert.equal(parseUnits('0.0000000001', 10), '1');
  assert.equal(parseUnits('999999.123456', 6), '999999123456');
});
test('amount entry never truncates, rounds or accepts exponent/sign syntax', () => {
  for (const value of ['1e3', '-1', '+1', '01', '1,000', ' 1', '1.', '0.00000000001', '1844674407.3709551616']) assert.throws(() => parseUnits(value, 10), undefined, value);
  assert.throws(() => parseUnits('0', 10));
  assert.throws(() => parseUnits('1', 13));
});
test('slippage minimum uses exact integer floor at a uint64 boundary', () => {
  assert.equal(minimumOutput(MAX.toString(), 50), ((MAX * 9950n) / 10000n).toString());
  assert.equal(minimumOutput('10001', 50), '9950');
  assert.equal(minimumOutput('99', 0), '99');
  assert.throws(() => minimumOutput('1', 50));
  assert.throws(() => minimumOutput('100', 5001));
});
