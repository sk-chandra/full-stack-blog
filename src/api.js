// Thin client for the hotel price API. Calls are proxied to the Express
// server (see vite.config.js) in dev and served same-origin in production.

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

export function fetchCities() {
  return getJSON('/api/cities').then((d) => d.cities);
}

export function searchHotels({ city, checkIn, checkOut, guests }) {
  const params = new URLSearchParams({ city, checkIn, checkOut, guests: String(guests) });
  return getJSON(`/api/hotels?${params.toString()}`);
}
