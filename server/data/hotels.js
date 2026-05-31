// Curated catalogue of real 5-star hotels across popular destinations.
//
// This dataset powers the bundled "sample" provider so the platform works
// fully offline / without third-party API credentials. Each entry carries a
// `baseRate` (typical nightly rate in the hotel's local currency) which the
// sample provider uses as the anchor for deterministic, date-aware pricing.
//
// When live providers (e.g. Amadeus) are configured, this catalogue is only
// used as a fallback for cities the live provider can't price.

export const CITIES = [
  { code: 'PAR', name: 'Paris', country: 'France', currency: 'EUR', lat: 48.8566, lon: 2.3522 },
  { code: 'LON', name: 'London', country: 'United Kingdom', currency: 'GBP', lat: 51.5074, lon: -0.1278 },
  { code: 'NYC', name: 'New York', country: 'United States', currency: 'USD', lat: 40.7128, lon: -74.006 },
  { code: 'DXB', name: 'Dubai', country: 'United Arab Emirates', currency: 'AED', lat: 25.2048, lon: 55.2708 },
  { code: 'TYO', name: 'Tokyo', country: 'Japan', currency: 'JPY', lat: 35.6762, lon: 139.6503 },
  { code: 'SIN', name: 'Singapore', country: 'Singapore', currency: 'SGD', lat: 1.3521, lon: 103.8198 },
  { code: 'ROM', name: 'Rome', country: 'Italy', currency: 'EUR', lat: 41.9028, lon: 12.4964 },
  { code: 'BCN', name: 'Barcelona', country: 'Spain', currency: 'EUR', lat: 41.3851, lon: 2.1734 },
];

// id is stable so prices stay deterministic across requests.
export const HOTELS = [
  // --- Paris ---
  { id: 'par-ritz', city: 'PAR', name: 'The Ritz Paris', neighborhood: 'Place Vendôme, 1st arr.', baseRate: 1450, rating: 4.8 },
  { id: 'par-crillon', city: 'PAR', name: 'Hôtel de Crillon, A Rosewood Hotel', neighborhood: 'Place de la Concorde', baseRate: 1180, rating: 4.7 },
  { id: 'par-bristol', city: 'PAR', name: 'Le Bristol Paris', neighborhood: 'Rue du Faubourg Saint-Honoré', baseRate: 1320, rating: 4.8 },
  { id: 'par-georgev', city: 'PAR', name: 'Four Seasons Hotel George V', neighborhood: 'Champs-Élysées, 8th arr.', baseRate: 1650, rating: 4.9 },
  { id: 'par-lutetia', city: 'PAR', name: 'Hôtel Lutetia', neighborhood: 'Saint-Germain-des-Prés', baseRate: 890, rating: 4.6 },
  { id: 'par-pavillon', city: 'PAR', name: 'Pavillon de la Reine', neighborhood: 'Le Marais', baseRate: 640, rating: 4.6 },

  // --- London ---
  { id: 'lon-savoy', city: 'LON', name: 'The Savoy', neighborhood: 'Strand, Covent Garden', baseRate: 720, rating: 4.7 },
  { id: 'lon-claridges', city: 'LON', name: "Claridge's", neighborhood: 'Mayfair', baseRate: 880, rating: 4.8 },
  { id: 'lon-shangri', city: 'LON', name: 'Shangri-La The Shard', neighborhood: 'London Bridge', baseRate: 690, rating: 4.7 },
  { id: 'lon-connaught', city: 'LON', name: 'The Connaught', neighborhood: 'Mayfair', baseRate: 950, rating: 4.8 },
  { id: 'lon-corinthia', city: 'LON', name: 'Corinthia London', neighborhood: 'Whitehall', baseRate: 760, rating: 4.7 },
  { id: 'lon-rosewood', city: 'LON', name: 'Rosewood London', neighborhood: 'Holborn', baseRate: 580, rating: 4.6 },

  // --- New York ---
  { id: 'nyc-plaza', city: 'NYC', name: 'The Plaza', neighborhood: 'Midtown, 5th Avenue', baseRate: 1090, rating: 4.6 },
  { id: 'nyc-stregis', city: 'NYC', name: 'The St. Regis New York', neighborhood: 'Midtown East', baseRate: 1250, rating: 4.7 },
  { id: 'nyc-baccarat', city: 'NYC', name: 'Baccarat Hotel', neighborhood: 'Midtown, 53rd St', baseRate: 1180, rating: 4.7 },
  { id: 'nyc-mandarin', city: 'NYC', name: 'Mandarin Oriental New York', neighborhood: 'Columbus Circle', baseRate: 1320, rating: 4.7 },
  { id: 'nyc-carlyle', city: 'NYC', name: 'The Carlyle', neighborhood: 'Upper East Side', baseRate: 860, rating: 4.5 },
  { id: 'nyc-beekman', city: 'NYC', name: 'The Beekman', neighborhood: 'Financial District', baseRate: 640, rating: 4.6 },

  // --- Dubai ---
  { id: 'dxb-burj', city: 'DXB', name: 'Burj Al Arab Jumeirah', neighborhood: 'Jumeirah Beach', baseRate: 4200, rating: 4.8 },
  { id: 'dxb-atlantis', city: 'DXB', name: 'Atlantis The Palm', neighborhood: 'Palm Jumeirah', baseRate: 1650, rating: 4.6 },
  { id: 'dxb-address', city: 'DXB', name: 'Address Downtown', neighborhood: 'Downtown Dubai', baseRate: 1450, rating: 4.7 },
  { id: 'dxb-armani', city: 'DXB', name: 'Armani Hotel Dubai', neighborhood: 'Burj Khalifa', baseRate: 1900, rating: 4.7 },
  { id: 'dxb-jumeirahbeach', city: 'DXB', name: 'Jumeirah Beach Hotel', neighborhood: 'Jumeirah', baseRate: 1280, rating: 4.6 },

  // --- Tokyo ---
  { id: 'tyo-aman', city: 'TYO', name: 'Aman Tokyo', neighborhood: 'Otemachi', baseRate: 165000, rating: 4.8 },
  { id: 'tyo-peninsula', city: 'TYO', name: 'The Peninsula Tokyo', neighborhood: 'Marunouchi', baseRate: 98000, rating: 4.7 },
  { id: 'tyo-mandarin', city: 'TYO', name: 'Mandarin Oriental Tokyo', neighborhood: 'Nihonbashi', baseRate: 110000, rating: 4.7 },
  { id: 'tyo-ritz', city: 'TYO', name: 'The Ritz-Carlton Tokyo', neighborhood: 'Roppongi', baseRate: 105000, rating: 4.7 },
  { id: 'tyo-park', city: 'TYO', name: 'Park Hyatt Tokyo', neighborhood: 'Shinjuku', baseRate: 88000, rating: 4.6 },

  // --- Singapore ---
  { id: 'sin-marina', city: 'SIN', name: 'Marina Bay Sands', neighborhood: 'Marina Bay', baseRate: 720, rating: 4.6 },
  { id: 'sin-raffles', city: 'SIN', name: 'Raffles Singapore', neighborhood: 'City Hall', baseRate: 1180, rating: 4.7 },
  { id: 'sin-fullerton', city: 'SIN', name: 'The Fullerton Hotel', neighborhood: 'Downtown Core', baseRate: 560, rating: 4.6 },
  { id: 'sin-capella', city: 'SIN', name: 'Capella Singapore', neighborhood: 'Sentosa Island', baseRate: 1320, rating: 4.8 },
  { id: 'sin-shangri', city: 'SIN', name: 'Shangri-La Singapore', neighborhood: 'Orchard', baseRate: 640, rating: 4.6 },

  // --- Rome ---
  { id: 'rom-edenrome', city: 'ROM', name: 'Hotel Eden – Dorchester Collection', neighborhood: 'Via Veneto', baseRate: 880, rating: 4.8 },
  { id: 'rom-derussie', city: 'ROM', name: 'Hotel de Russie, Rocco Forte', neighborhood: 'Piazza del Popolo', baseRate: 920, rating: 4.8 },
  { id: 'rom-stregis', city: 'ROM', name: 'The St. Regis Rome', neighborhood: 'Repubblica', baseRate: 760, rating: 4.6 },
  { id: 'rom-bulgari', city: 'ROM', name: 'Bulgari Hotel Roma', neighborhood: 'Campo Marzio', baseRate: 1650, rating: 4.8 },
  { id: 'rom-cavalieri', city: 'ROM', name: 'Rome Cavalieri, Waldorf Astoria', neighborhood: 'Monte Mario', baseRate: 620, rating: 4.6 },

  // --- Barcelona ---
  { id: 'bcn-arts', city: 'BCN', name: 'Hotel Arts Barcelona', neighborhood: 'Port Olímpic', baseRate: 590, rating: 4.6 },
  { id: 'bcn-majestic', city: 'BCN', name: 'Majestic Hotel & Spa', neighborhood: 'Passeig de Gràcia', baseRate: 520, rating: 4.6 },
  { id: 'bcn-wbarcelona', city: 'BCN', name: 'W Barcelona', neighborhood: 'Barceloneta Beach', baseRate: 480, rating: 4.5 },
  { id: 'bcn-mandarin', city: 'BCN', name: 'Mandarin Oriental Barcelona', neighborhood: 'Passeig de Gràcia', baseRate: 780, rating: 4.7 },
  { id: 'bcn-cottonhouse', city: 'BCN', name: 'Cotton House Hotel', neighborhood: 'Eixample', baseRate: 440, rating: 4.6 },
];

const cityByCode = new Map(CITIES.map((c) => [c.code, c]));
const cityByName = new Map(CITIES.map((c) => [c.name.toLowerCase(), c]));

export function findCity(query) {
  if (!query) return null;
  const q = String(query).trim().toLowerCase();
  return cityByCode.get(q.toUpperCase()) || cityByName.get(q) || null;
}

export function hotelsForCity(code) {
  return HOTELS.filter((h) => h.city === code);
}
