export const MAX = (1n << 64n) - 1n;
export function integer(text, positive = false) {
  if (typeof text !== 'string' || !/^(0|[1-9][0-9]{0,19})$/.test(text)) throw Error('Enter a whole number without signs or leading zeros.');
  const value = BigInt(text);
  if (value > MAX || (positive && value === 0n)) throw Error('Amount is outside the permitted range.');
  return value;
}
export function parseUnits(text, decimals, positive = true) {
  if (!Number.isInteger(decimals) || decimals < 0 || decimals > 12) throw Error('Unknown asset precision.');
  if (typeof text !== 'string' || !/^(0|[1-9][0-9]*)(\.[0-9]+)?$/.test(text) || text.length > 40) throw Error('Enter a decimal amount without signs or separators.');
  const [whole, fraction = ''] = text.split('.');
  if (fraction.length > decimals) throw Error(`This asset supports ${decimals} decimal places.`);
  const value = BigInt(whole) * 10n ** BigInt(decimals) + BigInt(fraction.padEnd(decimals, '0') || '0');
  return integer(value.toString(), positive).toString();
}
export function formatUnits(text, decimals) {
  const value = integer(text);
  if (!Number.isInteger(decimals) || decimals < 0 || decimals > 12) throw Error('Unknown asset precision.');
  if (!decimals) return value.toString();
  const digits = value.toString().padStart(decimals + 1, '0');
  const fraction = digits.slice(-decimals).replace(/0+$/, '');
  return digits.slice(0, -decimals) + (fraction ? '.' + fraction : '');
}
export function minimumOutput(text, bps) {
  if (!Number.isInteger(bps) || bps < 0 || bps > 5000) throw Error('Slippage must be between 0 and 50%.');
  const minimum = integer(text, true) * BigInt(10000 - bps) / 10000n;
  if (!minimum) throw Error('Minimum output rounds to zero; increase the amount.');
  return minimum.toString();
}
