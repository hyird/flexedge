import { useMemo, useRef, useState } from 'react'
import { ChartTooltip, Widget } from '@heroui-pro/react'
import { formatNumber, formatPercent, ratioOf } from '@/lib/format'
import { countryTraffic } from './country_traffic'
import type { WebsiteDashboardRanking } from './dashboard-schema'
import countries from './world_map_paths.json'

export function CountryTrafficMap({
  data,
}: {
  data: WebsiteDashboardRanking[]
}) {
  const traffic = useMemo(() => countryTraffic(data), [data])
  const map = useRef<HTMLDivElement>(null)
  const [hovered, setHovered] = useState<{
    code: string
    left: number
    top: number
  }>()
  const maximum = Math.max(
    1,
    ...[...traffic.values()].map((row) => row.request_count)
  )
  const total = [...traffic.values()].reduce(
    (sum, row) => sum + row.request_count,
    0
  )
  const ranked = [...traffic.values()].sort(
    (left, right) => right.request_count - left.request_count
  )
  const highlighted = hovered ? traffic.get(hovered.code) : undefined
  const selectedName = countries.find(
    (country) => country.code === hovered?.code
  )?.name
  const highlight = (code: string, clientX: number, clientY: number) => {
    const bounds = map.current?.getBoundingClientRect()
    if (!bounds) return
    setHovered({
      code,
      left: Math.max(
        0,
        Math.min(clientX - bounds.left + 12, bounds.width - 224)
      ),
      top: Math.max(
        0,
        Math.min(clientY - bounds.top + 12, bounds.height - 100)
      ),
    })
  }
  return (
    <Widget className='min-w-0 xl:col-span-2'>
      <Widget.Header>
        <Widget.Title>国家/地区访问分布</Widget.Title>
      </Widget.Header>
      <Widget.Content className='flex'>
        <div
          className={
            ranked.length
              ? 'grid min-w-0 flex-1 items-stretch gap-5 md:grid-cols-[minmax(0,2fr)_minmax(11rem,1fr)]'
              : 'min-w-0 flex-1'
          }
        >
          <div
            ref={map}
            className='relative flex min-w-0 flex-col justify-center'
          >
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
                    aria-label={`${country.name}：${formatNumber(item?.request_count ?? 0)}次，${formatPercent(ratioOf(item?.request_count ?? 0, total))}`}
                    onPointerMove={(event) =>
                      highlight(country.code, event.clientX, event.clientY)
                    }
                    onPointerLeave={() => setHovered(undefined)}
                    onFocus={(event) => {
                      if (!event.currentTarget.matches(':focus-visible')) return
                      const bounds = event.currentTarget.getBoundingClientRect()
                      highlight(
                        country.code,
                        bounds.left + bounds.width / 2,
                        bounds.top + bounds.height / 2
                      )
                    }}
                    onBlur={() => setHovered(undefined)}
                  >
                    <path
                      d={country.path}
                      fill={
                        hovered?.code === country.code
                          ? 'color-mix(in oklch, var(--accent) 85%, var(--foreground))'
                          : item
                            ? `color-mix(in oklch, var(--accent) ${intensity}%, var(--surface))`
                            : 'var(--surface-tertiary)'
                      }
                      stroke='var(--surface)'
                      strokeWidth={0.7}
                      vectorEffect='non-scaling-stroke'
                    />
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
            {hovered && (
              <div
                role='tooltip'
                className='pointer-events-none absolute z-10 w-52'
                style={{ left: hovered.left, top: hovered.top }}
              >
                <ChartTooltip>
                  <ChartTooltip.Header>{selectedName}</ChartTooltip.Header>
                  <ChartTooltip.Item>
                    <ChartTooltip.Indicator color='var(--chart-3)' />
                    <ChartTooltip.Label>访问次数</ChartTooltip.Label>
                    <ChartTooltip.Value>
                      {formatNumber(highlighted?.request_count ?? 0)}
                    </ChartTooltip.Value>
                  </ChartTooltip.Item>
                  <ChartTooltip.Item>
                    <ChartTooltip.Indicator color='var(--chart-1)' />
                    <ChartTooltip.Label>占比</ChartTooltip.Label>
                    <ChartTooltip.Value>
                      {formatPercent(
                        ratioOf(highlighted?.request_count ?? 0, total)
                      )}
                    </ChartTooltip.Value>
                  </ChartTooltip.Item>
                </ChartTooltip>
              </div>
            )}
            <div className='text-xs text-muted'>
              <span>近24小时请求量</span>
            </div>
          </div>
          {ranked.length > 0 && (
            <div className='flex min-w-0 items-start'>
              <div className='grid w-full grid-cols-[minmax(0,1fr)_auto_auto] gap-x-3 gap-y-2 text-sm tabular-nums'>
                <span className='text-xs text-muted'>国家/地区</span>
                <span className='text-right text-xs text-muted'>次数</span>
                <span
                  className='text-right text-xs text-muted'
                  title='占已定位请求的比例'
                >
                  占比
                </span>
                {ranked.slice(0, 8).map((row) => (
                  <div key={row.label} className='contents'>
                    <span className='truncate' title={row.label}>
                      {row.label}
                    </span>
                    <span className='text-right'>
                      {formatNumber(row.request_count)}
                    </span>
                    <span className='text-right text-muted'>
                      {formatPercent(ratioOf(row.request_count, total))}
                    </span>
                  </div>
                ))}
              </div>
            </div>
          )}
        </div>
      </Widget.Content>
    </Widget>
  )
}
