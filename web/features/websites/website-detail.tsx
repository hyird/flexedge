import { useState } from 'react'
import { useMutation, useQuery } from '@tanstack/react-query'
import { DataGrid, type DataGridColumn } from '@heroui-pro/react'
import { BarChart } from '@heroui-pro/react/bar-chart'
import { KPI } from '@heroui-pro/react/kpi'
import { Button, Card, Chip, Tabs, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { formatBytes, formatBytesPerSecond, formatDate } from '@/lib/format'
import { Drawer } from '@/components/drawer'
import { Notice } from '@/components/forms'
import { EmptyState, QueryNotice } from '@/components/page'
import { StatusChip } from '@/components/status-chip'
import type {
  WebsiteDashboardClientIpRanking,
  WebsiteDashboardRanking,
  WebsiteDashboardSeriesPoint,
} from './dashboard-schema'
import { websiteDetailQuery, probeWebsiteDns } from './data'
import type { Website } from './types'
import { useWebsiteDashboard } from './use-website-dashboard'

export function WebsiteDetail({
  website: initial,
  onClose,
  onEdit,
}: {
  website: Website
  onClose: () => void
  onEdit: (website: Website) => void
}) {
  const [section, setSection] = useState('runtime')
  const detail = useQuery({
    ...websiteDetailQuery(initial.id),
    enabled: section === 'runtime',
  })
  const website = detail.data ?? initial
  const probe = useMutation({
    mutationFn: () => probeWebsiteDns(website.id),
    onSuccess: () => {
      toast.success('DNS 探测已提交')
      void detail.refetch()
    },
    onError: (error) => toast.danger(apiErrorMessage(error)),
  })
  return (
    <Drawer
      title={website.config.name || website.access_domain}
      onClose={onClose}
      size='lg'
    >
      <div className='space-y-5'>
        <div className='flex flex-wrap items-center justify-between gap-3'>
          <div className='flex flex-wrap items-center gap-2'>
            <StatusChip status={website.status} />
            <Chip size='sm' variant='soft'>
              {website.cluster_name}
            </Chip>
            <code className='text-xs text-muted'>{website.access_domain}</code>
          </div>
          <Button size='sm' variant='secondary' onPress={() => onEdit(website)}>
            编辑配置
          </Button>
        </div>
        <Tabs
          selectedKey={section}
          onSelectionChange={(key) => setSection(String(key))}
          variant='secondary'
        >
          <Tabs.ListContainer>
            <Tabs.List aria-label='网站详情视图'>
              <Tabs.Tab id='runtime'>
                运行状态
                <Tabs.Indicator />
              </Tabs.Tab>
              <Tabs.Tab id='traffic'>
                流量统计
                <Tabs.Indicator />
              </Tabs.Tab>
            </Tabs.List>
          </Tabs.ListContainer>
          <Tabs.Panel id={section} className='space-y-5 pt-4'>
            {section === 'traffic' ? (
              <WebsiteTraffic websiteId={website.id} />
            ) : (
              <>
                <QueryNotice
                  query={detail}
                  onRetry={() => void detail.refetch()}
                />
                <div className='grid gap-3 sm:grid-cols-3'>
                  <KPI>
                    <KPI.Header>
                      <KPI.Title>已同步节点</KPI.Title>
                    </KPI.Header>
                    <KPI.Content>
                      <KPI.Value value={website.runtime.synced_node_count} />
                    </KPI.Content>
                    <KPI.Footer>
                      目标 {website.runtime.target_node_count} 个节点
                    </KPI.Footer>
                  </KPI>
                  <KPI>
                    <KPI.Header>
                      <KPI.Title>网站域名</KPI.Title>
                    </KPI.Header>
                    <KPI.Content>
                      <KPI.Value value={website.config.domains.length} />
                    </KPI.Content>
                    <KPI.Footer>
                      {website.config.https_enabled ? 'HTTPS 已启用' : 'HTTP'}
                    </KPI.Footer>
                  </KPI>
                  <KPI>
                    <KPI.Header>
                      <KPI.Title>源站</KPI.Title>
                    </KPI.Header>
                    <KPI.Content>
                      <KPI.Value value={website.config.origins.length} />
                    </KPI.Content>
                    <KPI.Footer>
                      默认组 {website.config.default_origin_group}
                    </KPI.Footer>
                  </KPI>
                </div>
                <section className='space-y-3'>
                  <div className='flex items-center justify-between gap-3'>
                    <h3 className='text-sm font-medium'>域名解析</h3>
                    <Button
                      size='sm'
                      variant='secondary'
                      onPress={() => probe.mutate()}
                      isPending={probe.isPending}
                    >
                      探测 DNS
                    </Button>
                  </div>
                  <DataGrid
                    aria-label='域名解析状态'
                    data={website.config.domains}
                    getRowId={(domain) => domain.id}
                    columns={[
                      {
                        id: 'hostname',
                        header: '域名',
                        accessorKey: 'hostname',
                        isRowHeader: true,
                        minWidth: 180,
                      },
                      {
                        id: 'mode',
                        header: 'DNS',
                        cell: (domain) =>
                          domain.dns_mode === 'managed' ? '托管' : '外部',
                      },
                      {
                        id: 'state',
                        header: '解析状态',
                        cell: (domain) => (
                          <StatusChip
                            status={
                              website.runtime.domain_states.find(
                                (state) => state.id === domain.id
                              )?.resolution_status ?? 'pending'
                            }
                          />
                        ),
                      },
                      {
                        id: 'verified',
                        header: '最近验证',
                        minWidth: 160,
                        cell: (domain) =>
                          formatDate(
                            website.runtime.domain_states.find(
                              (state) => state.id === domain.id
                            )?.last_verified_at
                          ),
                      },
                      {
                        id: 'error',
                        header: '备注',
                        minWidth: 200,
                        cell: (domain) =>
                          website.runtime.domain_states.find(
                            (state) => state.id === domain.id
                          )?.last_error || '—',
                      },
                    ]}
                  />
                </section>
                <section className='space-y-3'>
                  <h3 className='text-sm font-medium'>源站健康</h3>
                  {website.runtime.origin_states.length ? (
                    <DataGrid
                      aria-label='源站健康状态'
                      data={website.runtime.origin_states}
                      getRowId={(state) =>
                        `${state.node_id}:${state.origin_id}`
                      }
                      columns={[
                        {
                          id: 'node',
                          header: '节点',
                          accessorKey: 'node_name',
                          isRowHeader: true,
                          minWidth: 140,
                        },
                        {
                          id: 'origin',
                          header: '源站',
                          minWidth: 220,
                          cell: (state) => {
                            const origin = website.config.origins.find(
                              (item) => item.id === state.origin_id
                            )
                            return origin
                              ? `${origin.group} · ${origin.protocol}://${origin.host}:${origin.port}`
                              : state.origin_id
                          },
                        },
                        {
                          id: 'status',
                          header: '状态',
                          cell: (state) => <StatusChip status={state.status} />,
                        },
                        {
                          id: 'latency',
                          header: '延迟',
                          cell: (state) => (
                            <span className='tabular-nums'>
                              {state.latency_millis} ms
                            </span>
                          ),
                        },
                        {
                          id: 'checked',
                          header: '检查时间',
                          minWidth: 160,
                          cell: (state) =>
                            formatDate(
                              new Date(
                                state.checked_at_unix_millis
                              ).toISOString()
                            ),
                        },
                        {
                          id: 'error',
                          header: '最近错误',
                          cell: (state) => state.last_error || '—',
                          minWidth: 170,
                        },
                      ]}
                    />
                  ) : (
                    <EmptyState
                      title='等待节点上报'
                      description='节点应用网站配置后将上报源站健康状态。'
                    />
                  )}
                </section>
                {website.config.https_enabled && (
                  <section className='space-y-3'>
                    <h3 className='text-sm font-medium'>绑定证书</h3>
                    {website.certificates.length ? (
                      website.certificates.map((certificate) => (
                        <div
                          key={certificate.id}
                          className='flex flex-wrap items-center justify-between gap-2 text-sm'
                        >
                          <span>{certificate.domains.join('、')}</span>
                          <Chip
                            size='sm'
                            variant='soft'
                            color={certificate.usable ? 'success' : 'warning'}
                          >
                            {certificate.usable ? '可用' : '不可用'}
                          </Chip>
                        </div>
                      ))
                    ) : (
                      <Notice>HTTPS 已启用，尚未绑定可用证书。</Notice>
                    )}
                  </section>
                )}
                <div className='flex justify-end'>
                  <Button
                    size='sm'
                    variant='tertiary'
                    onPress={() => void detail.refetch()}
                    isPending={detail.isFetching}
                  >
                    刷新状态
                  </Button>
                </div>
              </>
            )}
          </Tabs.Panel>
        </Tabs>
      </div>
    </Drawer>
  )
}

function Trend({
  title,
  data,
  metric,
}: {
  title: string
  data: WebsiteDashboardSeriesPoint[]
  metric: 'request_count' | 'bandwidth_bps'
}) {
  return (
    <Card>
      <Card.Header>
        <Card.Title className='text-sm'>{title}</Card.Title>
      </Card.Header>
      <Card.Content>
        {data.length ? (
          <BarChart
            data={data.map((item) => ({
              label: item.timestamp.slice(5, 16).replace('T', ' '),
              value:
                metric === 'bandwidth_bps'
                  ? item.bandwidth_bps / 8
                  : item.request_count,
            }))}
            height={190}
          >
            <BarChart.Grid vertical={false} />
            <BarChart.XAxis dataKey='label' tickMargin={8} minTickGap={35} />
            <BarChart.YAxis
              width={65}
              tickFormatter={(value: number) =>
                metric === 'bandwidth_bps'
                  ? formatBytesPerSecond(value)
                  : String(value)
              }
            />
            <BarChart.Bar
              dataKey='value'
              name={metric === 'bandwidth_bps' ? '带宽（B/s）' : '请求量'}
              fill='var(--accent)'
              radius={[4, 4, 0, 0]}
            />
            <BarChart.Tooltip content={<BarChart.TooltipContent />} />
          </BarChart>
        ) : (
          <EmptyState
            title='暂无统计数据'
            description='收到访问记录后显示趋势。'
          />
        )}
      </Card.Content>
    </Card>
  )
}

type RankingRow = WebsiteDashboardRanking &
  Partial<Pick<WebsiteDashboardClientIpRanking, 'asn' | 'as_name'>>
function Ranking({ title, data }: { title: string; data: RankingRow[] }) {
  const columns: DataGridColumn<RankingRow>[] = [
    {
      id: 'label',
      header: title,
      accessorKey: 'label',
      isRowHeader: true,
      minWidth: 180,
      cell: (item) => (
        <div className='flex flex-col gap-1'>
          <span title={item.label} className='max-w-56 truncate'>
            {item.label || '—'}
          </span>
          {item.asn && (
            <span className='text-xs text-muted'>
              {item.asn} {item.as_name}
            </span>
          )}
        </div>
      ),
    },
    {
      id: 'requests',
      header: '请求量',
      cell: (item) => (
        <span className='tabular-nums'>
          {item.request_count.toLocaleString()}
        </span>
      ),
    },
    {
      id: 'bytes',
      header: '流量',
      cell: (item) => (
        <span className='tabular-nums'>{formatBytes(item.response_bytes)}</span>
      ),
    },
  ]
  return (
    <section className='min-w-0 space-y-2'>
      <h3 className='text-sm font-medium'>{title}</h3>
      {data.length ? (
        <DataGrid
          aria-label={title}
          data={data}
          getRowId={(item) => item.label}
          columns={columns}
        />
      ) : (
        <p className='py-5 text-center text-sm text-muted'>暂无数据</p>
      )}
    </section>
  )
}

function WebsiteTraffic({ websiteId }: { websiteId: string }) {
  const dashboard = useWebsiteDashboard(websiteId)
  const summary = dashboard.data?.summary
  if (!summary)
    return (
      <div className='space-y-3'>
        <Notice>{dashboard.error}</Notice>
        <EmptyState
          title={dashboard.error ? '统计连接中断' : '正在连接统计'}
          description='等待节点访问记录生成统计快照。'
          action={
            <Button size='sm' variant='secondary' onPress={dashboard.reconnect}>
              重新连接
            </Button>
          }
        />
      </div>
    )
  return (
    <div className='space-y-5'>
      {dashboard.error && (
        <div className='space-y-2'>
          <Notice>{dashboard.error}</Notice>
          <Button size='sm' variant='secondary' onPress={dashboard.reconnect}>
            重新连接
          </Button>
        </div>
      )}
      <div className='grid gap-3 sm:grid-cols-2 lg:grid-cols-3'>
        {[
          {
            label: '当前带宽',
            value: summary.current_bandwidth_bps / 8,
            format: formatBytesPerSecond,
          },
          {
            label: '今日峰值带宽',
            value: summary.today_peak_bps / 8,
            format: formatBytesPerSecond,
          },
          {
            label: '今日响应流量',
            value: summary.today_response_bytes,
            format: formatBytes,
          },
          {
            label: '今日独立 IP',
            value: summary.today_unique_ips,
            format: (value: number) => value.toLocaleString(),
          },
          {
            label: '本月峰值带宽',
            value: summary.current_month_peak_bps / 8,
            format: formatBytesPerSecond,
          },
          {
            label: '上月峰值带宽',
            value: summary.previous_month_peak_bps / 8,
            format: formatBytesPerSecond,
          },
        ].map((metric) => (
          <KPI key={metric.label}>
            <KPI.Header>
              <KPI.Title>{metric.label}</KPI.Title>
            </KPI.Header>
            <KPI.Content>
              <KPI.Value value={metric.value}>
                {() => metric.format(metric.value)}
              </KPI.Value>
            </KPI.Content>
          </KPI>
        ))}
      </div>
      <div className='grid gap-3 lg:grid-cols-2'>
        <Trend
          title='小时带宽'
          data={dashboard.data!.hourly}
          metric='bandwidth_bps'
        />
        <Trend
          title='每日请求量'
          data={dashboard.data!.daily}
          metric='request_count'
        />
      </div>
      <div className='grid gap-5 lg:grid-cols-2'>
        <Ranking title='状态码' data={dashboard.data!.status_codes} />
        <Ranking title='请求方法' data={dashboard.data!.methods} />
        <Ranking title='国家 / 地区' data={dashboard.data!.countries} />
        <Ranking title='域名' data={dashboard.data!.hosts} />
        <Ranking title='访问路径' data={dashboard.data!.paths} />
        <Ranking title='来源页面' data={dashboard.data!.referers} />
        <Ranking
          title='客户端 IP · 流量'
          data={dashboard.data!.client_ips_by_bytes}
        />
        <Ranking
          title='客户端 IP · 请求量'
          data={dashboard.data!.client_ips_by_requests}
        />
      </div>
    </div>
  )
}
