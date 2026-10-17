import { useMemo, useState } from 'react'
import { Widget } from '@heroui-pro/react'
import { formatBytes, formatNumber } from '@/lib/format'
import { countryTraffic } from './country_traffic'
import type { WebsiteDashboardRanking } from './dashboard-schema'
import countries from './world_map_paths.json'

export function CountryTrafficMap({
  data,
}: {
  data: WebsiteDashboardRanking[]
}) {
  const traffic = useMemo(() => countryTraffic(data), [data])
  const [hovered, setHovered] = useState<string>()
  const maximum = Math.max(
    1,
    ...[...traffic.values()].map((row) => row.request_count)
  )
  const highlighted = hovered ? traffic.get(hovered) : data[0]
  const selectedName = hovered
    ? countries.find((country) => country.code === hovered)?.name
    : data[0]?.label
  return (
    <Widget className='min-w-0'>
      <Widget.Header>
        <Widget.Title>国家/地区访问分布</Widget.Title>
      </Widget.Header>
      <Widget.Content>
        <div className='grid min-w-0 gap-5 xl:grid-cols-[minmax(0,3fr)_minmax(15rem,1fr)]'>
          <div className='min-w-0'>
            <svg
              viewBox='0 0 1000 460'
              className='w-full'
              role='img'
              aria-label='近24小时国家/地区请求量世界地图，颜色越深请求越多'
            >
              {countries.map((country) => {
                const item = traffic.get(country.code)
                const intensity = item
                  ? Math.min(
                      100,
                      20 +
                        (80 * Math.log1p(item.request_count)) /
                          Math.log1p(maximum)
                    )
                  : 0
                return (
                  <g
                    key={country.code}
                    className='outline-none'
                    tabIndex={item ? 0 : undefined}
                    onMouseEnter={() => setHovered(country.code)}
                    onMouseLeave={() => setHovered(undefined)}
                    onFocus={() => setHovered(country.code)}
                    onBlur={() => setHovered(undefined)}
                  >
                    <path
                      d={country.path}
                      fill={
                        item
                          ? `color-mix(in oklch, var(--accent) ${intensity}%, var(--surface))`
                          : 'var(--surface-tertiary)'
                      }
                      stroke={
                        hovered === country.code
                          ? 'var(--foreground)'
                          : 'var(--surface)'
                      }
                      strokeWidth={hovered === country.code ? 1.5 : 0.7}
                      vectorEffect='non-scaling-stroke'
                    >
                      <title>
                        {country.name}：
                        {item
                          ? `${formatNumber(item.request_count)}次请求，${formatBytes(item.response_bytes)}`
                          : '暂无定位记录'}
                      </title>
                    </path>
                    {country.marker && item && (
                      <circle
                        cx={country.marker[0]}
                        cy={country.marker[1]}
                        r={2.5}
                        fill='var(--accent)'
                        stroke='var(--surface)'
                        strokeWidth={0.6}
                      />
                    )}
                  </g>
                )
              })}
            </svg>
            <div className='flex flex-wrap justify-between gap-2 text-xs text-muted'>
              <span>近24小时 · 颜色越深，请求越多</span>
              <a
                href='https://www.naturalearthdata.com/'
                target='_blank'
                rel='noreferrer'
              >
                Natural Earth
              </a>
            </div>
          </div>
          <div className='flex min-w-0 flex-col gap-4'>
            <div className='rounded-xl bg-surface-secondary p-4 tabular-nums'>
              <p className='font-medium'>{selectedName ?? '暂无地区数据'}</p>
              <p className='mt-2 text-sm'>
                {highlighted
                  ? `${formatNumber(highlighted.request_count)}次请求`
                  : '暂无定位记录'}
              </p>
              <p className='mt-1 text-sm text-muted'>
                {highlighted ? formatBytes(highlighted.response_bytes) : '—'}
              </p>
            </div>
            <ol className='flex flex-col gap-2'>
              {data.slice(0, 8).map((row) => (
                <li
                  key={row.label}
                  className='flex min-w-0 justify-between gap-3 text-sm tabular-nums'
                >
                  <span className='truncate'>{row.label}</span>
                  <span className='shrink-0 text-muted'>
                    {formatNumber(row.request_count)}
                  </span>
                </li>
              ))}
            </ol>
            <p className='text-xs text-muted'>
              按访问日志中的客户端IP定位，无法定位的请求不显示在地图上。
            </p>
          </div>
        </div>
      </Widget.Content>
    </Widget>
  )
}
