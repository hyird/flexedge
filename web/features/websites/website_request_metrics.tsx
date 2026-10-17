import { Widget } from '@heroui-pro/react'
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
  new Intl.NumberFormat('zh-CN', { maximumFractionDigits: 2 }).format(value)
function Statistic({
  label,
  value,
  detail,
}: {
  label: string
  value: string
  detail: string
}) {
  return (
    <KPI className='h-26 min-w-0 gap-1 px-4 py-3'>
      <KPI.Header className='mb-0'>
        <KPI.Title>{label}</KPI.Title>
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
  const count = formatNumber
  const rate = (part: number) =>
    formatPercent(s.request_count > 0 ? part / s.request_count : undefined)
  const hourly = data.hourly.map((point) => ({
    label: formatDate(point.timestamp).slice(11, 16),
    qps: point.request_count / 3600,
    duration:
      point.request_count > 0 ? point.duration_ms / point.request_count : null,
    hits: point.cache_hit_requests,
  }))
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
          detail='最近1分钟平均请求数/秒'
        />
        <Statistic
          label='峰值QPS'
          value={decimal(s.peak_qps)}
          detail='近24小时分钟平均峰值'
        />
        <Statistic
          label='平均响应耗时'
          value={s.request_count ? `${decimal(s.average_duration_ms)}ms` : '—'}
          detail='按日志记录的请求处理耗时'
        />
        <Statistic
          label='P95响应耗时'
          value={s.request_count ? `${decimal(s.p95_duration_ms)}ms` : '—'}
          detail='95%的请求不超过此耗时'
        />
        <Statistic
          label='5xx错误率'
          value={rate(s.status_5xx_count)}
          detail={`5xx：${count(s.status_5xx_count)}次 · 4xx：${count(s.status_4xx_count)}次（${rate(s.status_4xx_count)}）`}
        />
      </div>
      <div className='grid grid-cols-2 gap-3 lg:grid-cols-3 2xl:grid-cols-6'>
        <Statistic
          label='请求命中率'
          value={formatPercent(hit.requests)}
          detail={`内存${count(s.cache_memory_requests)}次 · 磁盘${count(s.cache_disk_requests)}次`}
        />
        <Statistic
          label='流量命中率'
          value={formatPercent(hit.bytes)}
          detail='HIT响应流量/已记录缓存状态的响应流量'
        />
        <Statistic
          label='缓存命中流量'
          value={s.cache_known_requests ? formatBytes(s.cache_hit_bytes) : '—'}
          detail={`${count(s.cache_hit_requests)}次HIT`}
        />
        <Statistic
          label='校验复用'
          value={count(s.cache_revalidated_requests)}
          detail='REVALIDATED · 源站304后复用缓存'
        />
        <Statistic
          label='缓存未命中'
          value={count(s.cache_miss_requests)}
          detail='MISS · 节点没有可直接复用的缓存'
        />
        <Statistic
          label='绕过缓存'
          value={count(s.cache_bypass_requests)}
          detail='BYPASS · 按请求或规则直接处理'
        />
      </div>
      <p className='-mt-3 text-xs text-muted'>
        请求与缓存指标为近24小时。命中率仅统计有缓存状态的日志，REVALIDATED单独统计；
        {count(s.request_count - s.cache_known_requests)}
        条历史日志未记录缓存状态。 响应流量包含响应首部，不代表实际回源流量。
      </p>
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
                stroke='var(--chart-1)'
                fill='var(--chart-1)'
                fillOpacity={0.12}
                dot={false}
              />
              <AreaChart.Tooltip />
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
                width={64}
                tickFormatter={(value: number) => `${decimal(value)}ms`}
              />
              <AreaChart.Area
                dataKey='duration'
                name='平均耗时（ms）'
                stroke='var(--chart-2)'
                fill='var(--chart-2)'
                fillOpacity={0.12}
                dot={false}
                connectNulls={false}
              />
              <AreaChart.Tooltip />
            </AreaChart>
          </Widget.Content>
        </Widget>
      </div>
    </>
  )
}
