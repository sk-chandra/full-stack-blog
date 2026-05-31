// Amadeus Self-Service provider — LIVE 5-star hotel prices.
//
// Amadeus offers a genuinely public, free-tier developer API. To enable it,
// create an app at https://developers.amadeus.com and set:
//
//   AMADEUS_CLIENT_ID=...
//   AMADEUS_CLIENT_SECRET=...
//   AMADEUS_ENV=test            # "test" (default) or "production"
//
// Flow:
//   1. OAuth2 client_credentials  -> access token (cached until expiry)
//   2. reference-data/locations/hotels/by-city?ratings=5  -> hotelIds
//   3. v3/shopping/hotel-offers?hotelIds=...               -> live offers/prices
//
// If creds are missing the provider reports itself unconfigured and the server
// transparently falls back to the bundled sample provider.

import { CITIES, findCity } from '../data/hotels.js';

const HOSTS = {
  test: 'https://test.api.amadeus.com',
  production: 'https://api.amadeus.com',
};

function host() {
  return HOSTS[process.env.AMADEUS_ENV === 'production' ? 'production' : 'test'];
}

let tokenCache = { value: null, expiresAt: 0 };

async function getToken() {
  const now = Date.now();
  if (tokenCache.value && now < tokenCache.expiresAt - 30_000) {
    return tokenCache.value;
  }
  const res = await fetch(`${host()}/v1/security/oauth2/token`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      grant_type: 'client_credentials',
      client_id: process.env.AMADEUS_CLIENT_ID,
      client_secret: process.env.AMADEUS_CLIENT_SECRET,
    }),
  });
  if (!res.ok) {
    throw new Error(`Amadeus auth failed (${res.status})`);
  }
  const json = await res.json();
  tokenCache = {
    value: json.access_token,
    expiresAt: now + json.expires_in * 1000,
  };
  return tokenCache.value;
}

async function amadeusGet(path, params, token) {
  const url = new URL(`${host()}${path}`);
  Object.entries(params).forEach(([k, v]) => {
    if (v !== undefined && v !== null && v !== '') url.searchParams.set(k, v);
  });
  const res = await fetch(url, { headers: { Authorization: `Bearer ${token}` } });
  if (!res.ok) {
    const text = await res.text().catch(() => '');
    throw new Error(`Amadeus ${path} failed (${res.status}): ${text.slice(0, 200)}`);
  }
  return res.json();
}

// Amadeus keys hotels by IATA city code. Reuse the catalogue's codes where they
// match; otherwise fall back to a best-effort 3-letter code.
function cityCodeFor(resolved) {
  return resolved.code;
}

export const amadeusProvider = {
  name: 'amadeus',
  isConfigured() {
    return Boolean(process.env.AMADEUS_CLIENT_ID && process.env.AMADEUS_CLIENT_SECRET);
  },
  listCities() {
    return CITIES.map(({ code, name, country }) => ({ code, name, country }));
  },
  async search({ city, checkIn, checkOut, guests }) {
    const resolved = findCity(city);
    if (!resolved) {
      const err = new Error(`Unknown city "${city}".`);
      err.status = 400;
      throw err;
    }
    const token = await getToken();
    const cityCode = cityCodeFor(resolved);

    // 1) Find 5-star hotels in the city.
    const byCity = await amadeusGet(
      '/v1/reference-data/locations/hotels/by-city',
      { cityCode, ratings: '5', radius: 20, radiusUnit: 'KM' },
      token,
    );
    const hotelIds = (byCity.data || []).map((h) => h.hotelId).slice(0, 40);
    if (hotelIds.length === 0) {
      return { city: resolved.name, country: resolved.country, currency: resolved.currency, hotels: [] };
    }

    // 2) Price live offers for those hotels.
    const offers = await amadeusGet(
      '/v3/shopping/hotel-offers',
      {
        hotelIds: hotelIds.join(','),
        adults: guests,
        checkInDate: checkIn,
        checkOutDate: checkOut,
        bestRateOnly: true,
        currency: resolved.currency,
      },
      token,
    );

    const nights = Math.max(
      1,
      Math.round(
        (new Date(`${checkOut}T00:00:00Z`) - new Date(`${checkIn}T00:00:00Z`)) / 86_400_000,
      ),
    );

    const hotels = (offers.data || [])
      .filter((entry) => entry.available && entry.offers && entry.offers.length)
      .map((entry) => {
        const cheapest = entry.offers.reduce((min, o) =>
          Number(o.price.total) < Number(min.price.total) ? o : min,
        );
        const total = Number(cheapest.price.total);
        return {
          id: entry.hotel.hotelId,
          name: entry.hotel.name,
          stars: 5,
          rating: entry.hotel.rating ? Number(entry.hotel.rating) : null,
          neighborhood: entry.hotel.address?.lines?.join(', ') || '',
          city: resolved.name,
          country: resolved.country,
          currency: cheapest.price.currency || resolved.currency,
          avgNightly: Math.round(total / nights),
          total: Math.round(total),
          nights,
          source: 'amadeus',
        };
      });

    hotels.sort((a, b) => a.avgNightly - b.avgNightly);
    return { city: resolved.name, country: resolved.country, currency: resolved.currency, hotels };
  },
};
