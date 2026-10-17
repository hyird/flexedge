import { useMutation } from '@tanstack/react-query'
import { Link } from '@tanstack/react-router'
import { Lock, Magnifier } from '@gravity-ui/icons'
import { ListView, type DataGridColumn } from '@heroui-pro/react'
import { KPI } from '@heroui-pro/react/kpi'
import { Alert, Button } from '@heroui/react'
import { formatBytes, formatDate, ratioOf } from '@/lib/format'
import {
  describeStatus,
  statusReserve,
  type StatusDictionary,
} from '@/lib/status'
import { cn } from '@/lib/utils'
import { DataTable, TruncatedText } from '@/components/data-table'
import { Mono } from '@/components/mono'
import { EmptyState } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { DescriptionList, Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
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
export function WebsiteOverview() {
  const website = useWebsite()
  const openTab = useOpenWebsiteTab()
  const probe = useMutation({
    mutationFn: () => probeWebsiteDns(website.id),
    onSuccess: () =>
      toastWriteSuccess('DNS 探测已提交，结果会自动更新', {
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
          {...describeStatus(domainResolutionStatuses, resolution(domain))}
          reserve={domainResolutionReserve}
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
      id: 'origin-group',
      header: '源站组',
      minWidth: 100,
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
      minWidth: 220,
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
      minWidth: 100,
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
              : '仅 HTTP'}
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
          variant='secondary'
          columns={domainColumns}
          data={config.domains}
          isLoading={false}
          minWidth={760}
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
        <Section title='绑定证书'>
          {!config.https_enabled ? (
            <EmptyState
              size='sm'
              icon={<Lock />}
              title='未启用 HTTPS'
              description='节点只接受 HTTP 请求。'
            />
          ) : website.certificates.length ? (
            <ListView
              aria-label='绑定证书'
              variant='secondary'
              items={website.certificates}
            >
              {(certificate) => (
                <ListView.Item
                  id={certificate.id}
                  href={`/certificates/${certificate.id}`}
                  textValue={certificate.domains.join('、')}
                  className='rounded-lg'
                >
                  <ListView.ItemContent>
                    <ListView.Title
                      title={certificate.domains.join('、')}
                      className='font-mono text-[13px] font-normal'
                    >
                      {certificate.domains.join('、')}
                    </ListView.Title>
                  </ListView.ItemContent>
                  <ListView.ItemAction>
                    <StatusChip
                      {...certificateUsability[
                        certificate.usable ? 'usable' : 'unusable'
                      ]}
                      reserve={certificateReserve}
                    />
                  </ListView.ItemAction>
                </ListView.Item>
              )}
            </ListView>
          ) : (
            <Alert status='warning'>
              <Alert.Indicator />
              <Alert.Content>
                <Alert.Title>已启用 HTTPS，但尚未绑定可用证书</Alert.Title>
              </Alert.Content>
            </Alert>
          )}
        </Section>
        <Section title='配置摘要'>
          <DescriptionList
            items={[
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
