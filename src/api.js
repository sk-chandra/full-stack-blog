// Thin client for the hotel price API.
//
// Two modes:
//   - Default (dev / full deployment): calls the Express API under /api, which
//     is proxied in dev and served same-origin in production.
//   - Static mode (VITE_STATIC=true, e.g. GitHub Pages): there is no backend,
//     so prices are computed in the browser from the shared sample catalogue.

import { listCities as listCitiesLocal, searchSampleHotels } from '../shared/hotelData.js';

const STATIC = import.meta.env.VITE_STATIC === 'true';

async function getJSON(url) {
  const res = await fetch(url);
  let body = null;
  try {
    body = await res.json();
  } catch {
    // non-JSON response
  }
  if (!res.ok) {
    throw new Error(body?.error || `Request failed (${res.status})`);
  }
  return body;
}

const ISO_DATE = /^\d{4}-\d{2}-\d{2}$/;

function validate({ checkIn, checkOut, guests }) {
  if (!ISO_DATE.test(checkIn) || !ISO_DATE.test(checkOut)) {
    throw new Error('Dates must be valid.');
  }
  if (new Date(`${checkOut}T00:00:00Z`) <= new Date(`${checkIn}T00:00:00Z`)) {
    throw new Error('Check-out must be after check-in.');
  }
  if (!Number.isInteger(guests) || guests < 1 || guests > 10) {
    throw new Error('Guests must be between 1 and 10.');
  }
}

export function fetchCities() {
  if (STATIC) return Promise.resolve(listCitiesLocal());
  return getJSON('/api/cities').then((d) => d.cities);
}

export function searchHotels({ city, checkIn, checkOut, guests }) {
  if (STATIC) {
    return new Promise((resolve) => {
      validate({ checkIn, checkOut, guests });
      const result = searchSampleHotels({ city, checkIn, checkOut, guests });
      resolve({
        provider: 'sample',
        city: result.city,
        country: result.country,
        currency: result.currency,
        count: result.hotels.length,
        cheapest: result.hotels[0] || null,
        hotels: result.hotels,
      });
    });
  }
  const params = new URLSearchParams({ city, checkIn, checkOut, guests: String(guests) });
  return getJSON(`/api/hotels?${params.toString()}`);
}
