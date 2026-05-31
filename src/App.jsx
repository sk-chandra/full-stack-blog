import { useEffect, useState } from 'react';
import SearchBar from './components/SearchBar';
import HotelCard from './components/HotelCard';
import { fetchCities, searchHotels } from './api';
import { defaultDates, formatMoney, nightsBetween } from './utils';

const DEFAULTS = { city: 'Paris', guests: 2, ...defaultDates() };

export default function App() {
  const [cities, setCities] = useState([{ code: 'PAR', name: 'Paris', country: 'France' }]);
  const [results, setResults] = useState(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState(null);
  const [lastQuery, setLastQuery] = useState(DEFAULTS);

  // Load supported cities, then run an initial search so the page isn't empty.
  useEffect(() => {
    let cancelled = false;
    fetchCities()
      .then((list) => {
        if (!cancelled && list?.length) setCities(list);
      })
      .catch(() => {});
    runSearch(DEFAULTS);
    return () => {
      cancelled = true;
    };
  }, []);

  async function runSearch(query) {
    setLoading(true);
    setError(null);
    setLastQuery(query);
    try {
      const data = await searchHotels(query);
      setResults(data);
    } catch (err) {
      setError(err.message);
      setResults(null);
    } finally {
      setLoading(false);
    }
  }

  const nights = nightsBetween(lastQuery.checkIn, lastQuery.checkOut);

  return (
    <div className="min-h-screen bg-slate-50 text-slate-900">
      {/* Hero + search */}
      <header className="relative overflow-hidden bg-slate-900">
        <div
          className="absolute inset-0 opacity-30"
          style={{
            backgroundImage:
              'radial-gradient(60rem 30rem at 80% -10%, rgba(245,158,11,0.45), transparent), radial-gradient(40rem 24rem at 0% 0%, rgba(56,189,248,0.25), transparent)',
          }}
        />
        <div className="relative mx-auto max-w-6xl px-4 pb-24 pt-14 sm:px-6">
          <p className="text-sm font-semibold uppercase tracking-[0.2em] text-amber-400">
            FiveStar · Luxury for less
          </p>
          <h1 className="mt-3 max-w-2xl text-4xl font-extrabold leading-tight text-white sm:text-5xl">
            Find the cheapest 5-star hotel for your dates.
          </h1>
          <p className="mt-4 max-w-xl text-lg text-slate-300">
            We compare live nightly rates across the world&apos;s top luxury hotels and surface the
            best deal first.
          </p>
        </div>
      </header>

      <main className="mx-auto max-w-6xl px-4 sm:px-6">
        <div className="-mt-16">
          <SearchBar cities={cities} initial={DEFAULTS} loading={loading} onSearch={runSearch} />
        </div>

        {/* Summary bar */}
        {results && !loading && (
          <ResultsSummary results={results} nights={nights} />
        )}

        {/* States */}
        <section className="mt-6 pb-20">
          {error && (
            <div className="rounded-xl bg-red-50 p-5 text-red-700 ring-1 ring-red-200">
              <strong className="font-semibold">Couldn&apos;t load hotels.</strong> {error}
            </div>
          )}

          {loading && <SkeletonGrid />}

          {!loading && results && results.hotels.length === 0 && (
            <p className="rounded-xl bg-white p-8 text-center text-slate-500 ring-1 ring-slate-900/5">
              No 5-star availability found for these dates. Try a different destination or stay.
            </p>
          )}

          {!loading && results && results.hotels.length > 0 && (
            <div className="grid grid-cols-1 gap-5 sm:grid-cols-2 lg:grid-cols-3">
              {results.hotels.map((hotel, i) => (
                <HotelCard
                  key={hotel.id}
                  hotel={hotel}
                  nights={nights}
                  rank={i + 1}
                  best={i === 0}
                />
              ))}
            </div>
          )}
        </section>
      </main>

      <footer className="border-t border-slate-200 bg-white">
        <div className="mx-auto max-w-6xl px-4 py-6 text-sm text-slate-500 sm:px-6">
          Prices shown are {results?.provider === 'sample' ? 'representative sample data' : 'live rates'}
          {results?.provider ? ` · source: ${results.provider}` : ''}. Configure the Amadeus API to
          enable live pricing — see the README.
        </div>
      </footer>
    </div>
  );
}

function ResultsSummary({ results, nights }) {
  const { cheapest } = results;
  return (
    <div className="mt-6 flex flex-wrap items-center justify-between gap-4 rounded-xl bg-white p-5 ring-1 ring-slate-900/5">
      <div>
        <h2 className="text-xl font-bold text-slate-900">
          {results.count} five-star hotels in {results.city}
        </h2>
        <p className="text-sm text-slate-500">
          {results.country} · {nights} {nights === 1 ? 'night' : 'nights'}
        </p>
      </div>
      {cheapest && (
        <div className="rounded-lg bg-amber-50 px-4 py-2 text-right ring-1 ring-amber-200">
          <div className="text-xs font-semibold uppercase tracking-wide text-amber-700">
            Cheapest from
          </div>
          <div className="text-xl font-extrabold text-amber-900">
            {formatMoney(cheapest.avgNightly, cheapest.currency)}
            <span className="text-sm font-medium text-amber-700"> / night</span>
          </div>
        </div>
      )}
    </div>
  );
}

function SkeletonGrid() {
  return (
    <div className="grid grid-cols-1 gap-5 sm:grid-cols-2 lg:grid-cols-3">
      {Array.from({ length: 6 }).map((_, i) => (
        <div key={i} className="h-44 animate-pulse rounded-2xl bg-white ring-1 ring-slate-900/5" />
      ))}
    </div>
  );
}
