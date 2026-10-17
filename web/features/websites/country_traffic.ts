import type { WebsiteDashboardRanking } from './dashboard-schema'
import countries from './world_map_paths.json'

const chinese = new Intl.DisplayNames(['zh-CN'], { type: 'region' })
const english = new Intl.DisplayNames(['en'], { type: 'region' })
const regionByName = new Map<string, string>()
const nameByRegion = new Map(countries.map(({ code, name }) => [code, name]))
for (const country of countries) {
  regionByName.set(country.name, country.code)
  regionByName.set(country.code, country.code)
  if (/^[A-Z]{2}$/.test(country.code)) {
    regionByName.set(chinese.of(country.code) ?? country.name, country.code)
    regionByName.set(english.of(country.code) ?? country.name, country.code)
  }
}
// Common XDB labels differ from CLDR's region names.
for (const [name, code] of Object.entries({
  中国: 'CN',
  中华人民共和国: 'CN',
  美国: 'US',
  俄罗斯: 'RU',
  英国: 'GB',
  韩国: 'KR',
  中国香港: 'HK',
  中国澳门: 'MO',
  中国台湾: 'TW',
  香港: 'HK',
  澳门: 'MO',
  台湾: 'TW',
}))
  regionByName.set(name, code)

export function countryTraffic(rows: WebsiteDashboardRanking[]) {
  const result = new Map<string, WebsiteDashboardRanking>()
  for (const row of rows) {
    const code = regionByName.get(row.label)
    if (!code) continue
    const previous = result.get(code)
    result.set(code, {
      label: nameByRegion.get(code) ?? row.label,
      request_count: row.request_count + (previous?.request_count ?? 0),
      response_bytes: row.response_bytes + (previous?.response_bytes ?? 0),
    })
  }
  return result
}
