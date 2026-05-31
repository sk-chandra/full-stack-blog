// Re-export the shared catalogue so server modules keep a stable import path.
// The single source of truth lives in shared/hotelData.js (also used by the
// static frontend build).
export { CITIES, HOTELS, findCity, hotelsForCity } from '../../shared/hotelData.js';
