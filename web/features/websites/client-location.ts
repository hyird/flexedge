/** Compact geographic label shared by access logs and client rankings. */
export function formatClientLocation(value?: string) {
  return value?.replace(/\s*[·•]\s*/g, '').trim() || '未知'
}

/** Ranking labels contain the IP before the geographic segments. */
export function clientIpDisplay(value: string) {
  const [ip, ...location] = value.split(/\s*[·•]\s*/)
  const geographicLabel = location.length
    ? formatClientLocation(location.join(''))
    : undefined
  return {
    ip: ip.trim() || '未知',
    location: geographicLabel || '未知',
  }
}
