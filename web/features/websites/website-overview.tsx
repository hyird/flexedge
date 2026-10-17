import { useMutation, type UseQueryResult } from '@tanstack/react-query'
import { Link } from '@tanstack/react-router'
import { Magnifier } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { KPI } from '@heroui-pro/react/kpi'
import { Button } from '@heroui/react'
import { formatBytes, formatDate, ratioOf } from '@/lib/format'
import {
  describeStatus,
  statusReserve,
  type StatusDictionary,
} from '@/lib/status'
import { cn } from '@/lib/utils'
import { DataTable, TruncatedText } from '@/components/data-table'
import { Mono } from '@/components/mono'
import { RelativeTime } from '@/components/relative-time'
import { DescriptionList, Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import type { WebsiteDashboard } from './dashboard-schema'
import { probeWebsiteDns } from './data'
import type { Website, WebsiteDomain } from './types'
import { useOpenWebsiteTab, useWebsite } from './website-context'
import {
  defaultOrigin,
  deployStatuses,
  deployStatusReserve,
  domainResolutionReserve,
  domainResolutionStatuses,
  originAddress,
  originGroupName,
  originGroups,
  originHealth,
  originHealthReserve,
  originHealthStatuses,
  sortedOriginStates,
} from './website-display'
import { WebsiteAnalytics } from './website_analytics'

type OriginState = Website['runtime']['origin_states'][number]

/** Whether a bound certificate can serve HTTPS now. */
const certificateUsability = {
  usable: { label: '可用', tone: 'success' },
  unusable: { label: '不可用', tone: 'warning' },
} as const satisfies StatusDictionary
const certificateReserve = statusReserve(certificateUsability)

function kpiStatus(part: number, total: number) {
  if (total === 0 || part === total) return 'success' as const
  return part === 0 ? ('danger' as const) : ('warning' as const)
}

/** `/websites/$websiteId` overview: deployment, domains, origin health, certificates. */
export function WebsiteOverview({
  dashboard,
}: {
  dashboard: UseQueryResult<WebsiteDashboard>
}) {
  const website = useWebsite()
  const openTab = useOpenWebsiteTab()
  const probe = useMutation({
    mutationFn: () => probeWebsiteDns(website.id),
    onSuccess: () =>
      toastWriteSuccess('DNS探测已提交，结果会自动更新', {
        resourceType: 'website',
        resourceId: website.id,
      }),
  })
  const { runtime, config } = website
  const health = originHealth(website)
  const groups = originGroups(website)
  const origin = defaultOrigin(website)
  const domainState = (domain: WebsiteDomain) =>
    runtime.domain_states.find((state) => state.id === domain.id)
  // A domain without a probe result is unverified (website_runtime_mapper.h).
  const resolution = (domain: WebsiteDomain) =>
    domainState(domain)?.resolution_status ?? 'unverified'
  const resolved = config.domains.filter(
    (domain) => resolution(domain) === 'verified'
  ).length

  const domainColumns: DataGridColumn<WebsiteDomain>[] = [
    {
      id: 'hostname',
      header: '域名',
      isRowHeader: true,
      cell: (domain) => <Mono>{domain.hostname}</Mono>,
    },
    {
      id: 'mode',
      header: 'DNS',
      cell: (domain) => (domain.dns_mode === 'managed' ? '托管' : '外部'),
    },
    {
      id: 'protocol',
      header: '接入协议',
      cell: (domain) => (
        <span className='uppercase'>
          {domainState(domain)?.access_protocol || '—'}
        </span>
      ),
    },
    {
      id: 'state',
      header: '解析状态',
      cell: (domain) => (
        <StatusChip
          {...describeStatus(domainResolutionStatuses, resolution(domain))}
          reserve={domainResolutionReserve}
        />
      ),
    },
    {
      id: 'verified',
      header: '最近验证',
      cell: (domain) => (
        <RelativeTime value={domainState(domain)?.last_verified_at} />
      ),
    },
    {
      id: 'error',
      header: '备注',
      cell: (domain) => (
        <TruncatedText text={domainState(domain)?.last_error} />
      ),
    },
  ]
  const originColumns: DataGridColumn<OriginState>[] = [
    {
      id: 'node',
      header: '节点',
      isRowHeader: true,
      cell: (state) => (
        <Link
          to='/nodes/$nodeId'
          params={{ nodeId: state.node_id }}
          className='link'
        >
          {state.node_name}
        </Link>
      ),
    },
    {
      id: 'origin-group',
      header: '源站组',
      cell: (state) => {
        const item = config.origins.find(
          (value) => value.id === state.origin_id
        )
        return (
          <span className={cn('block truncate', !item && 'text-muted')}>
            {item ? originGroupName(item.group) : '已删除的源站'}
          </span>
        )
      },
    },
    {
      id: 'origin',
      header: '源站地址',
      cell: (state) => {
        const item = config.origins.find(
          (value) => value.id === state.origin_id
        )
        return (
          <Mono truncate>{item ? originAddress(item) : state.origin_id}</Mono>
        )
      },
    },
    {
      id: 'status',
      header: '状态',
      cell: (state) => (
        <StatusChip
          {...describeStatus(originHealthStatuses, state.status)}
          reserve={originHealthReserve}
        />
      ),
    },
    {
      id: 'latency',
      header: '延迟',
      cell: (state) => (
        <span className='tabular-nums'>{state.latency_millis} ms</span>
      ),
    },
    {
      id: 'checked',
      header: '检查时间',
      cell: (state) =>
        formatDate(new Date(state.checked_at_unix_millis).toISOString()),
    },
    {
      id: 'error',
      header: '最近错误',
      cell: (state) => <TruncatedText text={state.last_error} />,
    },
  ]

  return (
    <div className='flex flex-col gap-6'>
      <div className='grid gap-4 sm:grid-cols-3'>
        <KPI>
          <KPI.Header>
            <KPI.Title>已同步节点</KPI.Title>
          </KPI.Header>
          <KPI.Content>
            <KPI.Value
              value={runtime.synced_node_count}
              className='whitespace-nowrap'
            >
              {(formatted) => `${formatted} / ${runtime.target_node_count}`}
            </KPI.Value>
          </KPI.Content>
          <KPI.Progress
            value={
              ratioOf(runtime.synced_node_count, runtime.target_node_count) *
              100
            }
            status={kpiStatus(
              runtime.synced_node_count,
              runtime.target_node_count
            )}
          />
          <KPI.Footer className='text-xs text-muted'>
            <StatusChip
              {...describeStatus(deployStatuses, runtime.deploy_status)}
              reserve={deployStatusReserve}
            />
          </KPI.Footer>
        </KPI>
        <KPI>
          <KPI.Header>
            <KPI.Title>已解析域名</KPI.Title>
          </KPI.Header>
          <KPI.Content>
            <KPI.Value value={resolved} className='whitespace-nowrap'>
              {(formatted) => `${formatted} / ${config.domains.length}`}
            </KPI.Value>
          </KPI.Content>
          <KPI.Progress
            value={ratioOf(resolved, config.domains.length) * 100}
            status={kpiStatus(resolved, config.domains.length)}
            className={config.domains.length > 0 ? undefined : 'invisible'}
          />
          <KPI.Footer className='text-xs whitespace-nowrap text-muted'>
            {config.https_enabled
              ? `HTTPS · TLS ${config.minimum_tls_version}+`
              : '仅HTTP'}
          </KPI.Footer>
        </KPI>
        <KPI>
          <KPI.Header>
            <KPI.Title>健康源站</KPI.Title>
          </KPI.Header>
          <KPI.Content>
            <KPI.Value value={health.healthy} className='whitespace-nowrap'>
              {(formatted) =>
                health.total ? `${formatted} / ${health.total}` : '—'
              }
            </KPI.Value>
          </KPI.Content>
          <KPI.Progress
            value={ratioOf(health.healthy, health.total) * 100}
            status={kpiStatus(health.healthy, health.total)}
            className={health.total > 0 ? undefined : 'invisible'}
          />
          <KPI.Footer className='text-xs whitespace-nowrap text-muted'>
            {groups.length} 组 · {config.origins.length} 个源站
          </KPI.Footer>
        </KPI>
      </div>

      <Section
        title='域名解析'
        actions={
          <Button
            size='sm'
            variant='tertiary'
            isPending={probe.isPending}
            onPress={() => probe.mutate()}
          >
            <Magnifier />
            探测DNS
          </Button>
        }
      >
        <DataTable
          label='域名解析状态'
          variant='secondary'
          columns={domainColumns}
          data={config.domains}
          isLoading={false}
          empty={{
            title: '还没有绑定域名',
            description: '在配置的“域名”分组中添加域名。',
            action: (
              <Button
                size='sm'
                variant='tertiary'
                onPress={() => openTab('settings', { section: 'domains' })}
              >
                添加域名
              </Button>
            ),
          }}
        />
      </Section>

      <Section title='源站健康'>
        <DataTable
          label='源站健康状态'
          variant='secondary'
          columns={originColumns}
          data={sortedOriginStates(website)}
          getRowId={(state) => `${state.node_id}:${state.origin_id}`}
          isLoading={false}
          empty={{
            title: '等待节点上报',
            description: config.health_check_enabled
              ? '节点应用网站配置后会上报源站健康状态。'
              : '主动健康检查未开启，节点只在回源失败时上报。',
          }}
        />
      </Section>

      <Section title='配置摘要'>
        <DescriptionList
          columns={3}
          items={[
            {
              label: '绑定证书',
              value: website.certificates.length ? (
                <span className='flex flex-col gap-2'>
                  {website.certificates.map((certificate) => (
                    <span
                      key={certificate.id}
                      className='flex items-start gap-3'
                    >
                      <Link
                        to='/certificates/$certificateId'
                        params={{ certificateId: certificate.id }}
                        aria-label={certificate.domains.join('、')}
                        className='link min-w-0 flex-col items-start font-mono break-all'
                      >
                        {certificate.domains.map((domain) => (
                          <span key={domain} className='block'>
                            {domain}
                          </span>
                        ))}
                      </Link>
                      <StatusChip
                        {...certificateUsability[
                          certificate.usable ? 'usable' : 'unusable'
                        ]}
                        reserve={certificateReserve}
                      />
                    </span>
                  ))}
                </span>
              ) : (
                <span
                  className={config.https_enabled ? 'text-warning' : undefined}
                >
                  未绑定
                </span>
              ),
            },
            {
              label: '接入协议',
              value: config.https_enabled
                ? `HTTP/HTTPS，TLS${config.minimum_tls_version}+`
                : 'HTTP',
            },
            {
              label: 'HTTPS设置',
              value: config.https_enabled
                ? `强制HTTPS${config.force_https ? '开启' : '关闭'}，HSTS${config.hsts_enabled ? '开启' : '关闭'}`
                : '未启用',
            },
            {
              label: '默认源站',
              value: origin ? (
                <span className='flex flex-col gap-0.5'>
                  {originGroupName(origin.group)}
                  <Mono className='text-muted'>{originAddress(origin)}</Mono>
                </span>
              ) : (
                '未配置'
              ),
            },
            {
              label: '回源Host',
              value: config.origin_host_header ? (
                <Mono>{config.origin_host_header}</Mono>
              ) : (
                '跟随请求'
              ),
            },
            {
              label: '回源协议',
              value: { auto: '自动协商', http1: 'HTTP/1.1', http2: 'HTTP/2' }[
                config.origin_http_version
              ],
            },
            {
              label: '连接超时',
              value: `${config.origin_connect_timeout_seconds}秒`,
            },
            {
              label: '读取超时',
              value: `${config.origin_read_timeout_seconds}秒`,
            },
            {
              label: '传递客户端IP',
              value: config.pass_client_ip ? '已启用' : '未启用',
            },
            {
              label: '主动健康检查',
              value: config.health_check_enabled ? (
                <span className='flex flex-col gap-0.5'>
                  <Mono>{config.health_check_path}</Mono>
                  <span className='text-muted'>
                    每{config.health_check_interval_seconds}秒，超时
                    {config.health_check_timeout_seconds}秒，预期
                    {config.health_check_expected_status}
                  </span>
                </span>
              ) : (
                '未启用'
              ),
            },
            {
              label: '缓存策略',
              value:
                config.cache_enabled && config.cache_policy_id ? (
                  <Link
                    to='/cache/$policyId'
                    params={{ policyId: config.cache_policy_id }}
                    className='link'
                  >
                    已启用，查看策略
                  </Link>
                ) : (
                  '未启用'
                ),
            },
            {
              label: '路由规则',
              value: `${config.route_rules.filter((rule) => rule.status === 'enabled').length}条启用，共${config.route_rules.length}条`,
            },
            {
              label: '响应压缩',
              value: config.response_compression_enabled ? (
                <span className='flex flex-col gap-0.5'>
                  <Mono>
                    {config.response_compression_algorithms.join('/') ||
                      '已启用'}
                  </Mono>
                  <span className='text-muted'>
                    {formatBytes(config.response_compression_min_bytes)}–
                    {config.response_compression_max_bytes
                      ? formatBytes(config.response_compression_max_bytes)
                      : '不限'}
                  </span>
                </span>
              ) : (
                '未启用'
              ),
            },
            {
              label: '访问日志',
              value: config.access_log_enabled ? '已启用' : '未启用',
            },
            {
              label: '最近更新',
              value: <RelativeTime value={website.updated_at} />,
            },
          ]}
        />
      </Section>
      <WebsiteAnalytics dashboard={dashboard} />
    </div>
  )
}
