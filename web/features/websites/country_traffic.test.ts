import { expect, test } from 'vitest'
import { countryTraffic } from './country_traffic'
import countries from './world_map_paths.json'

test('joins XDB and CLDR country names and combines aliases without losing traffic', () => {
  const result = countryTraffic([
    { label: '中国', request_count: 2, response_bytes: 100 },
    { label: '中华人民共和国', request_count: 3, response_bytes: 200 },
    { label: 'United States', request_count: 4, response_bytes: 400 },
    { label: '新加坡', request_count: 1, response_bytes: 50 },
    { label: '未知地区', request_count: 6, response_bytes: 500 },
  ])
  expect(result.get('CN')).toMatchObject({
    request_count: 5,
    response_bytes: 300,
  })
  expect(result.get('US')?.request_count).toBe(4)
  expect(result.get('SG')?.request_count).toBe(1)
  expect(result.size).toBe(3)
})

test('includes small CDN regions and gives them visible map markers', () => {
  for (const code of ['HK', 'MO', 'SG']) {
    const country = countries.find((item) => item.code === code)
    expect(country?.path.startsWith('M')).toBe(true)
    expect(country?.marker).toHaveLength(2)
  }
  expect(new Set(countries.map((country) => country.code)).size).toBe(
    countries.length
  )
})
