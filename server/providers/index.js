// Provider selection + graceful fallback.
//
// Order of preference is configurable via HOTEL_PROVIDER (e.g. "amadeus" or
// "sample"). By default we prefer any configured live provider and fall back to
// the bundled sample data — both when nothing is configured and when a live
// call fails — so the platform always returns results.

import { amadeusProvider } from './amadeus.js';
import { sampleProvider } from './sample.js';

const ALL = [amadeusProvider, sampleProvider];

function preferredOrder() {
  const forced = process.env.HOTEL_PROVIDER;
  if (forced) {
    const match = ALL.find((p) => p.name === forced);
    if (match) return [match, sampleProvider];
  }
  // Live providers first (only if configured), sample last.
  return [...ALL.filter((p) => p.name !== 'sample'), sampleProvider];
}

export function activeProviderName() {
  const live = preferredOrder().find((p) => p.name !== 'sample' && p.isConfigured());
  return live ? live.name : 'sample';
}

export function listCities() {
  return sampleProvider.listCities();
}

export async function searchHotels(params) {
  let lastError = null;
  for (const provider of preferredOrder()) {
    if (!provider.isConfigured()) continue;
    try {
      const result = await provider.search(params);
      return { ...result, provider: provider.name };
    } catch (err) {
      // Client errors (bad city, bad dates) are not worth retrying elsewhere.
      if (err.status === 400) throw err;
      lastError = err;
      // fall through to the next provider (ultimately the sample fallback)
    }
  }
  throw lastError || new Error('No hotel provider available');
}
