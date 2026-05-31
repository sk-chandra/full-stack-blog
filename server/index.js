// Express API for the cheapest-5-star-hotels platform.
//
// Endpoints:
//   GET /api/health           -> { ok, provider }
//   GET /api/cities           -> supported destinations
//   GET /api/hotels?city=&checkIn=&checkOut=&guests=
//                             -> 5-star hotels for the city, cheapest first
//
// In production the built frontend (dist/) is served from the same origin.

import express from 'express';
import cors from 'cors';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';
import { searchHotels, listCities, activeProviderName } from './providers/index.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const app = express();
const PORT = process.env.PORT || 3001;

app.use(cors());
app.use(express.json());

const ISO_DATE = /^\d{4}-\d{2}-\d{2}$/;

function validateSearch(query) {
  const { city, checkIn, checkOut } = query;
  const guests = Number.parseInt(query.guests ?? '2', 10);
  const errors = [];

  if (!city) errors.push('`city` is required.');
  if (!ISO_DATE.test(checkIn || '')) errors.push('`checkIn` must be YYYY-MM-DD.');
  if (!ISO_DATE.test(checkOut || '')) errors.push('`checkOut` must be YYYY-MM-DD.');

  if (ISO_DATE.test(checkIn || '') && ISO_DATE.test(checkOut || '')) {
    if (new Date(`${checkOut}T00:00:00Z`) <= new Date(`${checkIn}T00:00:00Z`)) {
      errors.push('`checkOut` must be after `checkIn`.');
    }
  }
  if (!Number.isInteger(guests) || guests < 1 || guests > 10) {
    errors.push('`guests` must be between 1 and 10.');
  }
  return { errors, params: { city, checkIn, checkOut, guests } };
}

app.get('/api/health', (_req, res) => {
  res.json({ ok: true, provider: activeProviderName() });
});

app.get('/api/cities', (_req, res) => {
  res.json({ cities: listCities() });
});

app.get('/api/hotels', async (req, res) => {
  const { errors, params } = validateSearch(req.query);
  if (errors.length) {
    return res.status(400).json({ error: errors.join(' ') });
  }
  try {
    const result = await searchHotels(params);
    const cheapest = result.hotels[0] || null;
    return res.json({
      query: params,
      provider: result.provider,
      city: result.city,
      country: result.country,
      currency: result.currency,
      count: result.hotels.length,
      cheapest,
      hotels: result.hotels,
    });
  } catch (err) {
    const status = err.status || 502;
    return res.status(status).json({ error: err.message });
  }
});

// Serve the built SPA in production, with a client-side routing fallback.
const distDir = path.join(__dirname, '..', 'dist');
if (fs.existsSync(distDir)) {
  app.use(express.static(distDir));
  app.get('*', (_req, res) => res.sendFile(path.join(distDir, 'index.html')));
}

app.listen(PORT, () => {
  console.log(`Hotel price API listening on http://localhost:${PORT} (provider: ${activeProviderName()})`);
});
