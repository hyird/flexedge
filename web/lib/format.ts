export function formatDate(value?: string) {
  if (!value) return '—'
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return value
  const pad = (part: number) => String(part).padStart(2, '0')
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`
}

export function formatBytesPerSecond(value?: number) {
  if (value === undefined) return '—'
  const units = ['B/s', 'KB/s', 'MB/s', 'GB/s']
  let amount = value
  let index = 0
  while (amount >= 1024 && index < units.length - 1) {
    amount /= 1024
    index += 1
  }
  return `${amount.toFixed(index === 0 ? 0 : 1)} ${units[index]}`
}

export function initials(value?: string) {
  const text = value?.trim()
  if (!text) return 'FE'
  return text.slice(0, 2).toUpperCase()
}
