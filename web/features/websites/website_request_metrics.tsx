import { ChartTooltip, Widget } from '@heroui-pro/react'
import { AreaChart } from '@heroui-pro/react/area-chart'
import { KPI } from '@heroui-pro/react/kpi'
import {
  formatBytes,
  formatDate,
  formatNumber,
  formatPercent,
} from '@/lib/format'
import type { WebsiteDashboard } from './dashboard-schema'
import { cacheHitRates } from './traffic-statistics'

const decimal = (value: number) =>
  new Intl.NumberFormat('zh-CN', {
    minimumFractionDigits: 2,
    maximumFractionDigits: 2,
  }).format(value)
function Statistic({
  label,
  value,
  detail,
  hint,
}: {
  label: string
  value: string
  detail: string
  hint?: string
}) {
  return (
    <KPI className='h-26 min-w-0 gap-1 px-4 py-3'>
      <KPI.Header className='mb-0'>
        <KPI.Title>
          <span title={hint}>{label}</span>
        </KPI.Title>
      </KPI.Header>
      <KPI.Content className='min-h-0 grid-cols-1 content-start justify-items-start gap-y-1'>
        <p className='max-w-full truncate text-2xl font-semibold tabular-nums'>
          {value}
        </p>
        <span className='max-w-full truncate text-xs text-muted' title={detail}>
          {detail}
        </span>
      </KPI.Content>
    </KPI>
  )
}

export function WebsiteRequestMetrics({ data }: { data: WebsiteDashboard }) {
  const s = data.statistics
  const hit = cacheHitRates(s)
  const cacheHint = `近24小时，仅统计有缓存状态的日志；${formatNumber(s.request_count - s.cache_known_requests)}条未记录。REVALIDATED单独统计。流量包含响应首部。`
  const count = formatNumber
  const rate = (part: number) =>
    formatPercent(s.request_count > 0 ? part / s.request_count : undefined)
  return (
    <>
      <div className='grid grid-cols-2 gap-3 lg:grid-cols-3 2xl:grid-cols-6'>
        <Statistic
          label='24小时请求'
          value={count(s.request_count)}
          detail={`请求流量${formatBytes(s.request_bytes)}`}
        />
        <Statistic
          label='当前QPS'
          value={decimal(s.current_qps)}
          detail='近1分钟均值'
        />
        <Statistic
          label='峰值QPS'
          value={decimal(s.peak_qps)}
          detail='24小时分钟峰值'
        />
        <Statistic
          label='平均响应耗时'
          value={s.request_count ? `${decimal(s.average_duration_ms)}ms` : '—'}
          detail='平均处理耗时'
        />
        <Statistic
          label='P95响应耗时'
          value={s.request_count ? `${decimal(s.p95_duration_ms)}ms` : '—'}
          detail='95%请求耗时'
        />
        <Statistic
          label='错误率'
          hint='5xx请求占总请求的比例'
          value={rate(s.status_5xx_count)}
          detail={`4xx:${count(s.status_4xx_count)}次(${rate(s.status_4xx_count)}) · 5xx:${count(s.status_5xx_count)}次(${rate(s.status_5xx_count)})`}
        />
      </div>
      <div className='grid grid-cols-2 gap-3 lg:grid-cols-3 2xl:grid-cols-6'>
        <Statistic
          label='请求命中率'
          hint={cacheHint}
          value={formatPercent(hit.requests)}
          detail={`内存${count(s.cache_memory_requests)}次 · 磁盘${count(s.cache_disk_requests)}次`}
        />
        <Statistic
          label='流量命中率'
          hint={cacheHint}
          value={formatPercent(hit.bytes)}
          detail='HIT流量占比'
        />
        <Statistic
          label='缓存命中流量'
          value={s.cache_known_requests ? formatBytes(s.cache_hit_bytes) : '—'}
          detail={`${count(s.cache_hit_requests)}次HIT`}
        />
        <Statistic
          label='校验复用'
          value={count(s.cache_revalidated_requests)}
          detail='REVALIDATED'
        />
        <Statistic
          label='缓存未命中'
          value={count(s.cache_miss_requests)}
          detail='MISS'
        />
        <Statistic
          label='绕过缓存'
          value={count(s.cache_bypass_requests)}
          detail='BYPASS'
        />
      </div>
    </>
  )
}

export function WebsiteRequestCharts({ data }: { data: WebsiteDashboard }) {
  const hourly = data.hourly.map((point) => ({
    label: formatDate(point.timestamp).slice(11, 16),
    qps: point.request_count / 3600,
    duration:
      point.request_count > 0 ? point.duration_ms / point.request_count : null,
  }))
  return (
    <div className='grid gap-6 xl:grid-cols-2'>
      <Widget className='min-w-0'>
        <Widget.Header>
          <Widget.Title>24小时平均QPS</Widget.Title>
        </Widget.Header>
        <Widget.Content>
          <AreaChart data={hourly} height={220}>
            <AreaChart.Grid vertical={false} />
            <AreaChart.XAxis dataKey='label' minTickGap={24} />
            <AreaChart.YAxis width={64} tickFormatter={decimal} />
            <AreaChart.Area
              dataKey='qps'
              name='平均QPS'
              type='monotone'
              stroke='var(--chart-1)'
              fill='var(--chart-1)'
              fillOpacity={0.12}
              dot={false}
            />
            <AreaChart.Tooltip
              content={({ active, label, payload }) => {
                const value = payload?.[0]?.value
                if (!active || typeof value !== 'number') return null
                return (
                  <ChartTooltip>
                    <ChartTooltip.Header>{label}</ChartTooltip.Header>
                    <ChartTooltip.Item>
                      <ChartTooltip.Indicator color='var(--chart-1)' />
                      <ChartTooltip.Label>平均QPS</ChartTooltip.Label>
                      <ChartTooltip.Value>{decimal(value)}</ChartTooltip.Value>
                    </ChartTooltip.Item>
                  </ChartTooltip>
                )
              }}
            />
          </AreaChart>
        </Widget.Content>
      </Widget>
      <Widget className='min-w-0'>
        <Widget.Header>
          <Widget.Title>24小时响应耗时</Widget.Title>
        </Widget.Header>
        <Widget.Content>
          <AreaChart data={hourly} height={220}>
            <AreaChart.Grid vertical={false} />
            <AreaChart.XAxis dataKey='label' minTickGap={24} />
            <AreaChart.YAxis
              width='auto'
              allowDecimals={false}
              tickFormatter={(value: number) => `${formatNumber(value)}ms`}
            />
            <AreaChart.Area
              dataKey='duration'
              name='平均耗时'
              type='monotone'
              stroke='var(--chart-2)'
              fill='var(--chart-2)'
              fillOpacity={0.12}
              dot={false}
              connectNulls={false}
            />
            <AreaChart.Tooltip
              content={({ active, label, payload }) => {
                const value = payload?.[0]?.value
                if (!active || typeof value !== 'number') return null
                return (
                  <ChartTooltip>
                    <ChartTooltip.Header>{label}</ChartTooltip.Header>
                    <ChartTooltip.Item>
                      <ChartTooltip.Indicator color='var(--chart-2)' />
                      <ChartTooltip.Label>平均耗时</ChartTooltip.Label>
                      <ChartTooltip.Value>
                        {decimal(value)}ms
                      </ChartTooltip.Value>
                    </ChartTooltip.Item>
                  </ChartTooltip>
                )
              }}
            />
          </AreaChart>
        </Widget.Content>
      </Widget>
    </div>
  )
}
