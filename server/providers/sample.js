// Sample provider — bundled, offline data source.
//
// Wraps the shared catalogue + deterministic pricing (shared/hotelData.js) so
// the platform is fully functional without any third-party API keys or network
// access. It is always "configured" and acts as the universal fallback.

import { listCities, searchSampleHotels } from '../../shared/hotelData.js';

export const sampleProvider = {
  name: 'sample',
  isConfigured() {
    return true;
  },
  listCities() {
    return listCities();
  },
  async search(params) {
    return searchSampleHotels(params);
  },
};
