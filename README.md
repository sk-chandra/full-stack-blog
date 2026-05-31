# FiveStar — Cheapest 5-Star Hotel Deals

A full-stack platform that finds the **cheapest 5-star hotel** for a given
destination and date range. Search by city, check-in/check-out and party size;
results come back sorted cheapest-first with the best deal highlighted.

- **Frontend:** React 19 + Vite + Tailwind CSS
- **Backend:** Express API with a pluggable hotel-data **provider** architecture
- **Data:** live prices via the public [Amadeus Self-Service API](https://developers.amadeus.com)
  when credentials are configured, with a bundled offline dataset as a fallback

## Quick start

```bash
npm install
npm run dev
```

`npm run dev` runs both the Vite dev server (port **5173**) and the Express API
(port **3001**) concurrently. Open http://localhost:5173 — API calls under
`/api` are proxied to the backend automatically.

Other scripts:

| script           | what it does                                        |
| ---------------- | --------------------------------------------------- |
| `npm run dev`    | run web + API together (development)                |
| `npm run dev:web`| Vite dev server only                                |
| `npm run server` | Express API only                                    |
| `npm run build`  | build the frontend into `dist/`                     |
| `npm start`      | run the API and serve the built `dist/` same-origin |
| `npm run lint`   | ESLint                                              |

For production: `npm run build && npm start`, then open http://localhost:3001.

## How pricing works

The backend exposes a small JSON API and chooses a data **provider** at request
time (see `server/providers/`):

1. **`amadeus`** — *live data*. Fetches 5-star hotels for the city, then queries
   live offers and keeps the cheapest available rate per hotel. Used only when
   `AMADEUS_CLIENT_ID` / `AMADEUS_CLIENT_SECRET` are set.
2. **`sample`** — *offline fallback*. A curated catalogue of real 5-star hotels
   across 8 destinations with **deterministic, date-aware pricing** (seasonality,
   weekend premiums, length of stay, occupancy). Requires no network or keys, so
   the platform is fully functional out of the box.

If a live provider isn't configured — or a live call fails — the server
transparently falls back to the sample provider so a search never comes back
empty. The UI footer states which source produced the prices.

### Enabling live prices

Create a free app at https://developers.amadeus.com and set:

```bash
export AMADEUS_CLIENT_ID=your_id
export AMADEUS_CLIENT_SECRET=your_secret
export AMADEUS_ENV=test          # "test" (default) or "production"
# optional: force a provider regardless of config
export HOTEL_PROVIDER=amadeus
```

Restart the API and `/api/health` will report `"provider": "amadeus"`.

> **Note on this hosted sandbox:** Claude Code on the web runs behind a network
> allowlist. External hosts (Amadeus, OpenStreetMap, etc.) are blocked here, so
> the app serves the bundled sample data. Run it locally with the env vars above
> to get genuinely live rates.

## Deployment (public)

The app is published to **GitHub Pages** by the
[`Deploy to GitHub Pages`](.github/workflows/deploy-pages.yml) workflow on every
push to the development branch:

**Live URL:** https://sk-chandra.github.io/full-stack-blog/

Because Pages serves static files only, the workflow builds the frontend in
**static mode** (`VITE_STATIC=true`): there is no backend, so the sample
pricing runs client-side from `shared/hotelData.js`. The same workflow captures
UI screenshots (Playwright/Chromium) and uploads them as the **`app-screenshots`**
artifact on the workflow run.

> First run also auto-enables Pages (`actions/configure-pages` with
> `enablement: true`). If your org restricts that, enable it once under
> **Settings → Pages → Build and deployment → Source: GitHub Actions** and
> re-run the workflow.

To get the full stack (live Amadeus prices via the Express API) you need a host
that runs Node — e.g. Render/Railway/Fly — using `npm run build && npm start`.

## API reference

Base URL: `/api` (proxied in dev, same-origin in production).

| Method & path                                              | Description                                  |
| ---------------------------------------------------------- | -------------------------------------------- |
| `GET /api/health`                                          | `{ ok, provider }`                           |
| `GET /api/cities`                                          | supported destinations                       |
| `GET /api/hotels?city=&checkIn=&checkOut=&guests=`         | 5-star hotels for the city, cheapest first   |

`checkIn` / `checkOut` are `YYYY-MM-DD`; `checkOut` must be after `checkIn`;
`guests` is 1–10 (default 2).

Example:

```bash
curl "http://localhost:3001/api/hotels?city=Paris&checkIn=2026-06-14&checkOut=2026-06-16&guests=2"
```

```jsonc
{
  "provider": "sample",
  "city": "Paris",
  "currency": "EUR",
  "count": 6,
  "cheapest": { "name": "Pavillon de la Reine", "avgNightly": 740, "total": 1480, "nights": 2, "stars": 5 },
  "hotels": [ /* ...sorted cheapest first */ ]
}
```

## Project structure

```
server/
  index.js              Express app: validation, routes, static serving
  providers/
    index.js            provider selection + graceful fallback
    amadeus.js          live Amadeus Self-Service integration
    sample.js           offline deterministic pricing
  data/hotels.js        curated 5-star hotel catalogue
src/
  App.jsx               page layout, search state, result rendering
  api.js                fetch client for the API
  utils.js              currency / date helpers
  components/
    SearchBar.jsx       destination + dates + guests form
    HotelCard.jsx       per-hotel result card
```

## Adding a new provider

Implement the `{ name, isConfigured(), listCities(), search(params) }` shape in
`server/providers/`, returning hotels with `{ id, name, stars, currency,
avgNightly, total, nights }`, and register it in `server/providers/index.js`.
Web-scraper-based providers (e.g. parsing a public booking site's results page)
plug in the same way.
