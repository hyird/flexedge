import { useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { KPI } from '@heroui-pro/react/kpi'
import { Alert, Skeleton } from '@heroui/react'
import { formatBitsPerSecond, formatDate, formatNumber } from '@/lib/format'
import { Mono } from '@/components/mono'
import { RelativeTime } from '@/components/relative-time'
import { DescriptionList, Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { clusterOptionsQuery } from '@/features/clusters/data'
import { dnsZoneLinesQuery } from '@/features/dns-zones/data'
import { dnsLinePath } from '@/features/dns-zones/dns-lines'
import { nodeQuery } from './data'
import type { Node } from './types'

const route = getRouteApi('/_authenticated/nodes/$nodeId')

function LoadKpi({ label, value }: { label: string; value?: number }) {
  const ratio =
    value === undefined ? undefined : Math.min(Math.max(value, 0), 1)
  return (
    <KPI>
      <KPI.Header>
        <KPI.Title>{label}</KPI.Title>
      </KPI.Header>
      <KPI.Content>
        {ratio === undefined ? (
          <KPI.Value value={0}>{() => '—'}</KPI.Value>
        ) : (
          <>
            <KPI.Value
              maximumFractionDigits={1}
              style='percent'
              value={ratio}
            />
            <KPI.Progress
              value={ratio * 100}
              status={
                ratio >= 0.9 ? 'danger' : ratio >= 0.75 ? 'warning' : 'success'
              }
            />
          </>
        )}
      </KPI.Content>
    </KPI>
  )
}

function TextKpi({
  label,
  value,
  hint,
}: {
  label: string
  value: string
  hint?: string
}) {
  return (
    <KPI>
      <KPI.Header>
        <KPI.Title>{label}</KPI.Title>
      </KPI.Header>
      <KPI.Content>
        <KPI.Value value={0}>{() => value}</KPI.Value>
      </KPI.Content>
      {hint && <KPI.Footer className='text-xs text-muted'>{hint}</KPI.Footer>}
    </KPI>
  )
}

function Endpoints({ node }: { node: Node }) {
  const clusters = useQuery(clusterOptionsQuery)
  const zoneId = clusters.data?.find(
    (cluster) => cluster.id === node.cluster_id
  )?.dns_zone_id
  const lines = useQuery(dnsZoneLinesQuery(zoneId))
  return (
    <ul className='flex flex-col divide-y divide-separator'>
      {node.config.endpoints.map((endpoint) => (
        <li
          key={endpoint.id}
          className='flex flex-wrap items-center justify-between gap-x-4 gap-y-1 py-2.5 first:pt-0 last:pb-0'
        >
          <Mono>{endpoint.ip_address}</Mono>
          <span className='text-[13px] text-muted'>
            {lines.data
              ? dnsLinePath(lines.data, endpoint.line_code)
              : endpoint.line_code === 'default'
                ? '默认线路'
                : endpoint.line_code}
          </span>
        </li>
      ))}
    </ul>
  )
}

/** Node detail index tab: live load KPIs, runtime facts and service
 * endpoints. Values update in place from `node-runtime` patches. */
export function NodeOverview() {
  const { nodeId } = route.useParams()
  const { data: node } = useQuery(nodeQuery(nodeId))
  if (!node)
    return (
      <div role='status' aria-label='正在加载节点概览' className='grid gap-4'>
        <div className='grid grid-cols-2 gap-3 lg:grid-cols-5'>
          {Array.from({ length: 5 }, (_, index) => (
            <Skeleton key={index} className='h-28 rounded-2xl' />
          ))}
        </div>
        <Skeleton className='h-56 rounded-2xl' />
      </div>
    )
  const runtime = node.runtime
  const behind = runtime.applied_node_spec_revision < node.node_spec_revision
  return (
    <div className='flex flex-col gap-6'>
      {runtime.registration_status === 'pending' && (
        <Alert status='accent'>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Title>节点尚未接入</Alert.Title>
            <Alert.Description>
              在服务器上运行安装命令后，节点会自动注册并开始上报运行指标。可在右上角“更多操作”中查看接入凭据。
            </Alert.Description>
          </Alert.Content>
        </Alert>
      )}
      <div className='grid grid-cols-2 gap-3 lg:grid-cols-5'>
        <LoadKpi label='CPU' value={runtime.cpu_usage} />
        <LoadKpi label='内存' value={runtime.memory_usage} />
        <TextKpi
          label='出口带宽'
          value={formatBitsPerSecond(runtime.traffic_out_bps)}
        />
        <TextKpi
          label='连接数'
          value={formatNumber(runtime.connection_count)}
        />
        <TextKpi
          label='系统负载'
          value={
            runtime.load_1m === undefined ? '—' : runtime.load_1m.toFixed(2)
          }
          hint='1 分钟平均'
        />
      </div>
      {runtime.last_error && (
        <Alert status='danger'>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Title>节点最近错误</Alert.Title>
            <Alert.Description className='break-words'>
              {runtime.last_error}
            </Alert.Description>
          </Alert.Content>
        </Alert>
      )}
      <div className='grid gap-6 xl:grid-cols-[minmax(0,2fr)_minmax(0,1fr)]'>
        <Section title='运行状态' description='来自节点心跳，实时更新。'>
          <DescriptionList
            columns={3}
            items={[
              {
                label: '连接',
                value: <StatusChip status={runtime.connection_status} />,
              },
              {
                label: '注册',
                value: (
                  <StatusChip
                    status={runtime.registration_status}
                    label={
                      runtime.registration_status === 'pending'
                        ? '等待接入'
                        : undefined
                    }
                  />
                ),
              },
              {
                label: '最近心跳',
                value: (
                  <span className='flex flex-col'>
                    <RelativeTime value={runtime.last_heartbeat_at} />
                    <span className='text-xs text-muted'>
                      {formatDate(runtime.last_heartbeat_at)}
                    </span>
                  </span>
                ),
              },
              {
                label: 'Agent 版本',
                value: runtime.agent_version ? (
                  <Mono>{runtime.agent_version}</Mono>
                ) : (
                  '—'
                ),
              },
              {
                label: '配置修订（已生效 / 目标）',
                value: (
                  <span className='flex flex-wrap items-center gap-2 tabular-nums'>
                    {runtime.applied_node_spec_revision} /{' '}
                    {node.node_spec_revision}
                    {behind && <StatusChip status='pending' label='待生效' />}
                  </span>
                ),
              },
              { label: '健康', value: runtime.health || '—' },
              {
                label: '排队日志',
                value: formatNumber(runtime.queued_log_events),
              },
              {
                label: '丢弃日志',
                value: formatNumber(runtime.dropped_log_events),
              },
              {
                label: '活动发布',
                value: runtime.active_release_id ? (
                  <Mono truncate>{runtime.active_release_id}</Mono>
                ) : (
                  '—'
                ),
              },
              {
                label: '清单摘要',
                wide: true,
                value: runtime.active_manifest_digest ? (
                  <Mono className='break-all'>
                    {runtime.active_manifest_digest}
                  </Mono>
                ) : (
                  '—'
                ),
              },
            ]}
          />
        </Section>
        <Section
          title='服务端点'
          description='节点对外服务的 IP 与对应 DNS 线路。'
        >
          <Endpoints node={node} />
        </Section>
      </div>
    </div>
  )
}
