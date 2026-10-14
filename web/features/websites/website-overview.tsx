import { useMutation } from '@tanstack/react-query'
import { Link } from '@tanstack/react-router'
import { Magnifier } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { KPI } from '@heroui-pro/react/kpi'
import { Button, toast } from '@heroui/react'
import { formatBytes, formatDate, ratioOf } from '@/lib/format'
import { statusLabels } from '@/lib/status'
import { DataTable, TruncatedText } from '@/components/data-table'
import { LinkButton } from '@/components/link-button'
import { Mono } from '@/components/mono'
import { RelativeTime } from '@/components/relative-time'
import { DescriptionList, Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { probeWebsiteDns } from './data'
import type { Website, WebsiteDomain } from './types'
import { useWebsite } from './website-context'
import {
  defaultOrigin,
  deployStatusReserve,
  originAddress,
  originGroups,
  originHealth,
} from './website-display'

type OriginState = Website['runtime']['origin_states'][number]

const resolutionReserve = statusLabels([
  'verified',
  'unverified',
  'invalid',
  'pending',
  'resolved',
])
const originStatusReserve = statusLabels(['healthy', 'unhealthy', 'unknown'])
const certificateReserve = ['可用', '不可用']

function kpiStatus(part: number, total: number) {
  if (total === 0 || part === total) return 'success' as const
  return part === 0 ? ('danger' as const) : ('warning' as const)
}

/** `/websites/$websiteId` overview: deployment, domains, origin health, certificates. */
export function WebsiteOverview() {
  const website = useWebsite()
  const probe = useMutation({
    mutationFn: () => probeWebsiteDns(website.id),
    onSuccess: () => toast.success('DNS 探测已提交，结果会自动更新'),
  })
  const { runtime, config } = website
  const health = originHealth(website)
  const groups = originGroups(website)
  const origin = defaultOrigin(website)
  const settings = `/websites/${website.id}?tab=settings`
  const domainState = (domain: WebsiteDomain) =>
    runtime.domain_states.find((state) => state.id === domain.id)
  const resolved = config.domains.filter(
    (domain) => domainState(domain)?.resolution_status === 'resolved'
  ).length

  const domainColumns: DataGridColumn<WebsiteDomain>[] = [
    {
      id: 'hostname',
      header: '域名',
      isRowHeader: true,
      minWidth: 200,
      cell: (domain) => <Mono>{domain.hostname}</Mono>,
    },
    {
      id: 'mode',
      header: 'DNS',
      minWidth: 90,
      cell: (domain) => (domain.dns_mode === 'managed' ? '托管' : '外部'),
    },
    {
      id: 'protocol',
      header: '接入协议',
      minWidth: 90,
      cell: (domain) => (
        <span className='uppercase'>
          {domainState(domain)?.access_protocol || '—'}
        </span>
      ),
    },
    {
      id: 'state',
      header: '解析状态',
      minWidth: 110,
      cell: (domain) => (
        <StatusChip
          status={domainState(domain)?.resolution_status ?? 'pending'}
          reserve={resolutionReserve}
        />
      ),
    },
    {
      id: 'verified',
      header: '最近验证',
      minWidth: 120,
      cell: (domain) => (
        <RelativeTime value={domainState(domain)?.last_verified_at} />
      ),
    },
    {
      id: 'error',
      header: '备注',
      minWidth: 200,
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
      minWidth: 140,
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
      id: 'origin',
      header: '源站',
      minWidth: 240,
      cell: (state) => {
        const item = config.origins.find(
          (value) => value.id === state.origin_id
        )
        return (
          <div className='flex min-w-0 flex-col gap-0.5'>
            <span className={item ? 'truncate' : 'truncate text-muted'}>
              {item ? item.group : '已删除的源站'}
            </span>
            <Mono truncate className='text-xs text-muted'>
              {item ? originAddress(item) : state.origin_id}
            </Mono>
          </div>
        )
      },
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 100,
      cell: (state) => (
        <StatusChip status={state.status} reserve={originStatusReserve} />
      ),
    },
    {
      id: 'latency',
      header: '延迟',
      minWidth: 80,
      cell: (state) => (
        <span className='tabular-nums'>{state.latency_millis} ms</span>
      ),
    },
    {
      id: 'checked',
      header: '检查时间',
      minWidth: 160,
      cell: (state) =>
        formatDate(new Date(state.checked_at_unix_millis).toISOString()),
    },
    {
      id: 'error',
      header: '最近错误',
      minWidth: 180,
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
              status={runtime.deploy_status}
              reserve={deployStatusReserve}
            />
          </KPI.Footer>
        </KPI>
        <KPI>
          <KPI.Header>
            <KPI.Title>域名</KPI.Title>
          </KPI.Header>
          <KPI.Content>
            <KPI.Value
              value={config.domains.length}
              className='whitespace-nowrap'
            />
          </KPI.Content>
          <KPI.Footer className='text-xs whitespace-nowrap text-muted'>
            已解析 {resolved} 个 ·{' '}
            {config.https_enabled
              ? `HTTPS · TLS ${config.minimum_tls_version}+`
              : '仅 HTTP'}
          </KPI.Footer>
        </KPI>
        <KPI>
          <KPI.Header>
            <KPI.Title>源站组 / 健康</KPI.Title>
          </KPI.Header>
          <KPI.Content>
            <KPI.Value value={groups.length} className='whitespace-nowrap'>
              {(formatted) =>
                health.total
                  ? `${formatted} 组 · ${health.healthy}/${health.total} 健康`
                  : `${formatted} 组`
              }
            </KPI.Value>
          </KPI.Content>
          <KPI.Progress
            value={ratioOf(health.healthy, health.total) * 100}
            status={kpiStatus(health.healthy, health.total)}
            className={health.total > 0 ? undefined : 'invisible'}
          />
          <KPI.Footer className='text-xs whitespace-nowrap text-muted'>
            {health.total
              ? `${config.origins.length} 个源站`
              : '等待节点上报健康状态'}
          </KPI.Footer>
        </KPI>
      </div>

      <Section
        title='域名解析'
        description='托管域名由系统写入 DNS 记录；外部域名需要自行解析到接入域名。'
        actions={
          <Button
            size='sm'
            variant='tertiary'
            isPending={probe.isPending}
            onPress={() => probe.mutate()}
          >
            <Magnifier />
            探测 DNS
          </Button>
        }
      >
        <DataTable
          label='域名解析状态'
          columns={domainColumns}
          data={config.domains}
          isLoading={false}
          minWidth={760}
          empty={{
            title: '还没有绑定域名',
            description: '在配置的“域名”分组中添加域名。',
            action: (
              <LinkButton href={`${settings}?section=domains`}>
                添加域名
              </LinkButton>
            ),
          }}
        />
      </Section>

      <Section
        title='源站健康'
        description='节点上报的回源健康检查结果，实时更新。'
      >
        <DataTable
          label='源站健康状态'
          columns={originColumns}
          data={runtime.origin_states}
          getRowId={(state) => `${state.node_id}:${state.origin_id}`}
          isLoading={false}
          minWidth={900}
          empty={{
            title: '等待节点上报',
            description: config.health_check_enabled
              ? '节点应用网站配置后会上报源站健康状态。'
              : '主动健康检查未开启，节点只在回源失败时上报。',
          }}
        />
      </Section>

      <div className='grid gap-6 lg:grid-cols-2'>
        <Section
          title='绑定证书'
          actions={
            <LinkButton variant='tertiary' href={`${settings}?section=https`}>
              管理 HTTPS
            </LinkButton>
          }
        >
          {!config.https_enabled ? (
            <p className='text-sm text-muted'>
              未启用 HTTPS，节点只接受 HTTP 请求。
            </p>
          ) : website.certificates.length ? (
            <ul className='flex flex-col divide-y divide-separator'>
              {website.certificates.map((certificate) => (
                <li
                  key={certificate.id}
                  className='grid grid-cols-[minmax(0,1fr)_auto] items-center gap-2 py-2.5 first:pt-0 last:pb-0'
                >
                  <Link
                    to='/certificates/$certificateId'
                    params={{ certificateId: certificate.id }}
                    title={certificate.domains.join('、')}
                    className='link min-w-0 truncate font-mono text-[13px]'
                  >
                    {certificate.domains.join('、')}
                  </Link>
                  <StatusChip
                    label={certificate.usable ? '可用' : '不可用'}
                    tone={certificate.usable ? 'success' : 'warning'}
                    reserve={certificateReserve}
                  />
                </li>
              ))}
            </ul>
          ) : (
            <p className='text-sm text-warning'>
              已启用 HTTPS，但尚未绑定可用证书。
            </p>
          )}
        </Section>
        <Section title='配置摘要'>
          <DescriptionList
            items={[
              {
                label: '默认源站',
                value: origin ? (
                  <span className='flex flex-col gap-0.5'>
                    {origin.group}
                    <Mono className='text-muted'>{originAddress(origin)}</Mono>
                  </span>
                ) : (
                  '未配置'
                ),
              },
              {
                label: '回源 Host',
                value: config.origin_host_header ? (
                  <Mono>{config.origin_host_header}</Mono>
                ) : (
                  '跟随请求'
                ),
              },
              {
                label: '响应压缩',
                value: config.response_compression_enabled
                  ? `${config.response_compression_algorithms.join(' / ') || '已启用'} · ≥ ${formatBytes(config.response_compression_min_bytes)}`
                  : '未启用',
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
      </div>
    </div>
  )
}
