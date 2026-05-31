import { formatMoney } from '../utils';

function Stars() {
  return (
    <span className="text-amber-400" aria-label="5 star hotel">
      {'★★★★★'}
    </span>
  );
}

export default function HotelCard({ hotel, nights, rank, best }) {
  return (
    <article
      className={`relative flex flex-col justify-between rounded-2xl bg-white p-5 ring-1 transition hover:-translate-y-0.5 hover:shadow-lg ${
        best ? 'ring-2 ring-amber-400 shadow-lg shadow-amber-500/10' : 'ring-slate-900/5 shadow-sm'
      }`}
    >
      {best && (
        <span className="absolute -top-3 left-5 rounded-full bg-amber-500 px-3 py-1 text-xs font-bold uppercase tracking-wide text-white shadow">
          Cheapest deal
        </span>
      )}

      <div>
        <div className="flex items-start justify-between gap-3">
          <div>
            <Stars />
            <h3 className="mt-1 text-lg font-bold leading-tight text-slate-900">{hotel.name}</h3>
            <p className="text-sm text-slate-500">{hotel.neighborhood || hotel.city}</p>
          </div>
          {hotel.rating != null && (
            <span className="shrink-0 rounded-lg bg-slate-900 px-2 py-1 text-sm font-bold text-white">
              {hotel.rating.toFixed(1)}
            </span>
          )}
        </div>
      </div>

      <div className="mt-5 flex items-end justify-between border-t border-slate-100 pt-4">
        <div>
          <div className="text-2xl font-extrabold text-slate-900">
            {formatMoney(hotel.avgNightly, hotel.currency)}
            <span className="ml-1 text-sm font-medium text-slate-400">/ night</span>
          </div>
          <div className="text-sm text-slate-500">
            {formatMoney(hotel.total, hotel.currency)} total · {nights} {nights === 1 ? 'night' : 'nights'}
          </div>
        </div>
        <span className="text-xs font-semibold text-slate-400">#{rank}</span>
      </div>
    </article>
  );
}
