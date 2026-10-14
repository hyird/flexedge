const placeholder = '—'

/** Parses a backend timestamp (also zone-less or short `+08` offsets);
 * undefined when empty or invalid. */
export function parseDate(value?: string): Date | undefined {
  if (!value) return undefined
  // Older payloads omit the zone (implicit +08:00) or use a short "+08" offset.
  const normalizedValue =
    /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?$/.test(value)
      ? `${value}+08:00`
      : value.replace(/([+-]\d{2})$/, '$1:00')
  const date = new Date(normalizedValue)
  return Number.isNaN(date.getTime()) ? undefined : date
}

function dateParts(date: Date, timeZone: string) {
  return Object.fromEntries(
    new Intl.DateTimeFormat('zh-CN', {
      timeZone,
      year: 'numeric',
      month: '2-digit',
      day: '2-digit',
      hour: '2-digit',
      minute: '2-digit',
      second: '2-digit',
      hourCycle: 'h23',
    })
      .formatToParts(date)
      .map(({ type, value: part }) => [type, part])
  )
}

/** Local console time (Asia/Shanghai), e.g. `2026-09-05 00:25:10`. */
export function formatDate(value?: string) {
  if (!value) return placeholder
  const date = parseDate(value)
  if (!date) return value
  const parts = dateParts(date, 'Asia/Shanghai')
  return `${parts.year}-${parts.month}-${parts.day} ${parts.hour}:${parts.minute}:${parts.second}`
}

/** Absolute UTC time for tooltips and audit context, e.g. `2026-09-04 16:25:10 UTC`. */
export function formatUtcDate(value?: string) {
  if (!value) return placeholder
  const date = parseDate(value)
  if (!date) return value
  const parts = dateParts(date, 'UTC')
  return `${parts.year}-${parts.month}-${parts.day} ${parts.hour}:${parts.minute}:${parts.second} UTC`
}

const relativeUnits: Array<[Intl.RelativeTimeFormatUnit, number]> = [
  ['year', 365 * 24 * 3600],
  ['month', 30 * 24 * 3600],
  ['day', 24 * 3600],
  ['hour', 3600],
  ['minute', 60],
]

/** Relative time against `now`, e.g. `3 分钟前`, `2 天后`; under 45 s is `刚刚`. */
export function formatRelative(value?: string, now: number = Date.now()) {
  if (!value) return placeholder
  const date = parseDate(value)
  if (!date) return value
  const seconds = Math.round((date.getTime() - now) / 1000)
  if (Math.abs(seconds) < 45) return '刚刚'
  const formatter = new Intl.RelativeTimeFormat('zh-CN', { numeric: 'always' })
  for (const [unit, size] of relativeUnits)
    if (Math.abs(seconds) >= size)
      return formatter.format(Math.trunc(seconds / size), unit)
  return formatter.format(
    Math.trunc(seconds / 60) || Math.sign(seconds),
    'minute'
  )
}

function scaled(value: number, base: number, units: string[]) {
  let amount = Math.max(0, value)
  let index = 0
  while (amount >= base && index < units.length - 1) {
    amount /= base
    index += 1
  }
  return `${amount.toFixed(index === 0 ? 0 : 1)} ${units[index]}`
}

/** Storage and transfer sizes in binary units (1 KB = 1024 B). */
export function formatBytes(value?: number) {
  if (value === undefined || !Number.isFinite(value)) return placeholder
  return scaled(value, 1024, ['B', 'KB', 'MB', 'GB', 'TB', 'PB'])
}

/** Network bandwidth for every `*_bps` field (bits per second, decimal units). */
export function formatBitsPerSecond(value?: number) {
  if (value === undefined || !Number.isFinite(value)) return placeholder
  return scaled(value, 1000, ['bps', 'Kbps', 'Mbps', 'Gbps', 'Tbps'])
}

/** Counts with grouping (`12,345`); `compact` uses 万/亿 for KPI values. */
export function formatNumber(value?: number, options?: { compact?: boolean }) {
  if (value === undefined || !Number.isFinite(value)) return placeholder
  return new Intl.NumberFormat('zh-CN', {
    notation: options?.compact ? 'compact' : 'standard',
    maximumFractionDigits: options?.compact ? 1 : 0,
  }).format(value)
}

/** Ratio in [0, 1] rendered as a percentage, e.g. `0.124` → `12.4%`. */
export function formatPercent(ratio?: number, fractionDigits = 1) {
  if (ratio === undefined || !Number.isFinite(ratio)) return placeholder
  return new Intl.NumberFormat('zh-CN', {
    style: 'percent',
    maximumFractionDigits: fractionDigits,
  }).format(ratio)
}

/** Safe ratio for percentages and meters; an empty total yields 0. */
export function ratioOf(part: number, total: number) {
  return total > 0 ? part / total : 0
}
