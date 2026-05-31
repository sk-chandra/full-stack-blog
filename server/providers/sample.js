// Sample provider — bundled, offline data source.
//
// Produces realistic, *deterministic* nightly prices for the curated 5-star
// hotel catalogue so the platform is fully functional without any third-party
// API keys or network access. Prices are anchored on each hotel's baseRate and
// modulated by seasonality, weekend demand, length of stay and occupancy, plus
// a stable per-hotel pseudo-random "live availability" wobble.

import { CITIES, findCity, hotelsForCity } from '../data/hotels.js';

// Deterministic hash → float in [0, 1). Same inputs always yield same output,
// so a given (hotel, date) combination always returns the same price.
function seededUnit(str) {
  let h = 2166136261;
  for (let i = 0; i < str.length; i += 1) {
    h ^= str.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  // map to [0,1)
  return ((h >>> 0) % 100000) / 100000;
}

function eachNight(checkIn, checkOut) {
  const nights = [];
  const start = new Date(`${checkIn}T00:00:00Z`);
  const end = new Date(`${checkOut}T00:00:00Z`);
  for (let d = new Date(start); d < end; d.setUTCDate(d.getUTCDate() + 1)) {
    nights.push(new Date(d));
  }
  return nights;
}

// Seasonality multiplier by month (1-12). Summer + December peak.
const SEASON = [0.85, 0.85, 0.95, 1.0, 1.08, 1.18, 1.25, 1.22, 1.05, 1.0, 0.9, 1.15];

function nightlyRate(hotel, date, guests) {
  const month = date.getUTCMonth(); // 0-11
  const dow = date.getUTCDay(); // 0 Sun .. 6 Sat
  const weekend = dow === 5 || dow === 6 ? 1.12 : 1.0; // Fri/Sat premium
  const season = SEASON[month];
  // Stable per-night availability wobble: ±12%.
  const wobble = 0.88 + seededUnit(`${hotel.id}:${date.toISOString().slice(0, 10)}`) * 0.24;
  // Larger parties may need a bigger room category.
  const occupancy = guests > 2 ? 1 + (guests - 2) * 0.18 : 1;
  const raw = hotel.baseRate * season * weekend * wobble * occupancy;
  return Math.round(raw);
}

function priceHotel(hotel, { checkIn, checkOut, guests }) {
  const nights = eachNight(checkIn, checkOut);
  const perNight = nights.map((d) => nightlyRate(hotel, d, guests));
  const total = perNight.reduce((a, b) => a + b, 0);
  const avgNightly = Math.round(total / nights.length);
  const city = CITIES.find((c) => c.code === hotel.city);
  return {
    id: hotel.id,
    name: hotel.name,
    stars: 5,
    rating: hotel.rating,
    neighborhood: hotel.neighborhood,
    city: city.name,
    country: city.country,
    currency: city.currency,
    avgNightly,
    total,
    nights: nights.length,
    source: 'sample',
  };
}

export const sampleProvider = {
  name: 'sample',
  // Always available — it's the offline fallback.
  isConfigured() {
    return true;
  },
  listCities() {
    return CITIES.map(({ code, name, country }) => ({ code, name, country }));
  },
  async search({ city, checkIn, checkOut, guests }) {
    const resolved = findCity(city);
    if (!resolved) {
      const supported = CITIES.map((c) => c.name).join(', ');
      const err = new Error(`Unknown city "${city}". Supported cities: ${supported}.`);
      err.status = 400;
      throw err;
    }
    const hotels = hotelsForCity(resolved.code).map((h) =>
      priceHotel(h, { checkIn, checkOut, guests }),
    );
    hotels.sort((a, b) => a.avgNightly - b.avgNightly);
    return {
      city: resolved.name,
      country: resolved.country,
      currency: resolved.currency,
      hotels,
    };
  },
};
