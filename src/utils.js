// Formatting + date helpers shared across the UI.

export function formatMoney(amount, currency) {
  if (amount == null) return '—';
  try {
    return new Intl.NumberFormat(undefined, {
      style: 'currency',
      currency,
      maximumFractionDigits: 0,
    }).format(amount);
  } catch {
    // Unknown currency code — fall back to plain number + code.
    return `${currency} ${Math.round(amount).toLocaleString()}`;
  }
}

export function toISODate(date) {
  return date.toISOString().slice(0, 10);
}

// Default search window: check in 14 days from now, stay 2 nights.
export function defaultDates() {
  const checkIn = new Date();
  checkIn.setDate(checkIn.getDate() + 14);
  const checkOut = new Date(checkIn);
  checkOut.setDate(checkOut.getDate() + 2);
  return { checkIn: toISODate(checkIn), checkOut: toISODate(checkOut) };
}

export function nightsBetween(checkIn, checkOut) {
  const ms = new Date(`${checkOut}T00:00:00Z`) - new Date(`${checkIn}T00:00:00Z`);
  return Math.max(1, Math.round(ms / 86_400_000));
}
