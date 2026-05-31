import { useState } from 'react';
import { toISODate } from '../utils';

const todayISO = toISODate(new Date());

export default function SearchBar({ cities, initial, loading, onSearch }) {
  const [city, setCity] = useState(initial.city);
  const [checkIn, setCheckIn] = useState(initial.checkIn);
  const [checkOut, setCheckOut] = useState(initial.checkOut);
  const [guests, setGuests] = useState(initial.guests);

  function submit(e) {
    e.preventDefault();
    // Keep checkout strictly after checkin.
    const safeCheckOut = checkOut > checkIn ? checkOut : addDay(checkIn);
    onSearch({ city, checkIn, checkOut: safeCheckOut, guests: Number(guests) });
  }

  function addDay(iso) {
    const d = new Date(`${iso}T00:00:00Z`);
    d.setUTCDate(d.getUTCDate() + 1);
    return toISODate(d);
  }

  return (
    <form
      onSubmit={submit}
      className="grid grid-cols-1 gap-4 rounded-2xl bg-white p-5 shadow-xl shadow-slate-900/10 ring-1 ring-slate-900/5 sm:grid-cols-2 lg:grid-cols-[1.4fr_1fr_1fr_0.8fr_auto]"
    >
      <Field label="Destination">
        <select
          value={city}
          onChange={(e) => setCity(e.target.value)}
          className="w-full rounded-lg border-0 bg-slate-50 px-3 py-2.5 text-slate-900 ring-1 ring-inset ring-slate-200 focus:ring-2 focus:ring-amber-500"
        >
          {cities.map((c) => (
            <option key={c.code} value={c.name}>
              {c.name}, {c.country}
            </option>
          ))}
        </select>
      </Field>

      <Field label="Check-in">
        <input
          type="date"
          value={checkIn}
          min={todayISO}
          onChange={(e) => setCheckIn(e.target.value)}
          className="w-full rounded-lg border-0 bg-slate-50 px-3 py-2.5 text-slate-900 ring-1 ring-inset ring-slate-200 focus:ring-2 focus:ring-amber-500"
        />
      </Field>

      <Field label="Check-out">
        <input
          type="date"
          value={checkOut}
          min={checkIn}
          onChange={(e) => setCheckOut(e.target.value)}
          className="w-full rounded-lg border-0 bg-slate-50 px-3 py-2.5 text-slate-900 ring-1 ring-inset ring-slate-200 focus:ring-2 focus:ring-amber-500"
        />
      </Field>

      <Field label="Guests">
        <select
          value={guests}
          onChange={(e) => setGuests(e.target.value)}
          className="w-full rounded-lg border-0 bg-slate-50 px-3 py-2.5 text-slate-900 ring-1 ring-inset ring-slate-200 focus:ring-2 focus:ring-amber-500"
        >
          {[1, 2, 3, 4, 5, 6].map((n) => (
            <option key={n} value={n}>
              {n} {n === 1 ? 'guest' : 'guests'}
            </option>
          ))}
        </select>
      </Field>

      <div className="flex items-end">
        <button
          type="submit"
          disabled={loading}
          className="h-[42px] w-full rounded-lg bg-amber-500 px-6 font-semibold text-white shadow-sm transition hover:bg-amber-600 focus:outline-none focus:ring-2 focus:ring-amber-500 focus:ring-offset-2 disabled:cursor-not-allowed disabled:opacity-60 lg:w-auto"
        >
          {loading ? 'Searching…' : 'Find deals'}
        </button>
      </div>
    </form>
  );
}

function Field({ label, children }) {
  return (
    <label className="flex flex-col gap-1.5">
      <span className="text-xs font-semibold uppercase tracking-wide text-slate-500">{label}</span>
      {children}
    </label>
  );
}
