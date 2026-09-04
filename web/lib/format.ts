export function formatDate(value?: string) {
  if (!value) return '—'
  const normalizedValue =
    /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?$/.test(value)
      ? `${value}+08:00`
      : value.replace(/([+-]\d{2})$/, '$1:00')
  const date = new Date(normalizedValue)
  if (Number.isNaN(date.getTime())) return value
  const parts = Object.fromEntries(
    new Intl.DateTimeFormat('zh-CN', {
      timeZone: 'Asia/Shanghai',
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
  return `${parts.year}-${parts.month}-${parts.day} ${parts.hour}:${parts.minute}:${parts.second}`
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
