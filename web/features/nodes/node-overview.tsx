import { useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { KPI } from '@heroui-pro/react/kpi'
import { Alert, Skeleton } from '@heroui/react'
import { formatBitsPerSecond, formatNumber } from '@/lib/format'
import { TruncatedText } from '@/components/data-table'
import { Mono } from '@/components/mono'
import { DescriptionList, Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { clusterOptionsQuery } from '@/features/clusters/data'
import { dnsZoneLinesQuery } from '@/features/dns-zones/data'
import { dnsLinePath } from '@/features/dns-zones/dns-lines'
import { nodeQuery } from './data'
import { NodeConnectionChip } from './node-connection-chip'
import {
  liveMetrics,
  nodeHealthLabels,
  nodeHealthStatuses,
} from './node-status'
import type { Node } from './types'

const route = getRouteApi('/_authenticated/nodes/$nodeId')
// The configuration row shows whether the node runs its target spec, never
// the revision numbers themselves.
const specStateLabels = ['已生效', '待生效']

/** Load ratio KPI. The progress bar is always rendered (hidden without a
 * value) so the card keeps its height when a node goes offline. */
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
          <KPI.Value maximumFractionDigits={1} style='percent' value={ratio} />
        )}
        <KPI.Progress
          aria-hidden={ratio === undefined || undefined}
          className={ratio === undefined ? 'invisible' : undefined}
          value={(ratio ?? 0) * 100}
          status={
            ratio === undefined || ratio < 0.75
              ? 'success'
              : ratio >= 0.9
                ? 'danger'
                : 'warning'
          }
        />
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
  hint: string
}) {
  return (
    <KPI>
      <KPI.Header>
        <KPI.Title>{label}</KPI.Title>
      </KPI.Header>
      <KPI.Content>
        <KPI.Value value={0}>{() => value}</KPI.Value>
      </KPI.Content>
      <KPI.Footer className='truncate text-xs text-muted'>{hint}</KPI.Footer>
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
          className='grid grid-cols-[minmax(0,auto)_minmax(0,1fr)] items-center gap-x-4 py-2.5 first:pt-0 last:pb-0'
        >
          <Mono truncate>{endpoint.ip_address}</Mono>
          <span className='truncate text-right text-[13px] text-muted'>
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
        <div className='grid grid-cols-2 gap-3 md:grid-cols-3 xl:grid-cols-6'>
          {Array.from({ length: 6 }, (_, index) => (
            <Skeleton key={index} className='h-28 rounded-2xl' />
          ))}
        </div>
        <Skeleton className='h-56 rounded-2xl' />
      </div>
    )
  const runtime = node.runtime
  const metrics = liveMetrics(runtime)
  const pending = runtime.applied_node_spec_revision < node.node_spec_revision
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
      <div className='grid grid-cols-2 gap-3 md:grid-cols-3 xl:grid-cols-6'>
        <LoadKpi label='CPU' value={metrics.cpu_usage} />
        <LoadKpi label='内存' value={metrics.memory_usage} />
        <TextKpi
          label='带宽'
          value={formatBitsPerSecond(metrics.traffic_out_bps)}
          hint='实时'
        />
        <TextKpi
          label='连接数'
          value={formatNumber(metrics.connection_count)}
          hint='当前活动连接'
        />
        <TextKpi
          label='连接 IP'
          value={formatNumber(metrics.connected_ip_count)}
          hint='当前连接的客户端 IP'
        />
        <TextKpi
          label='系统负载'
          value={
            metrics.load_1m === undefined ? '—' : metrics.load_1m.toFixed(2)
          }
          hint='1 分钟平均'
        />
      </div>
      <div className='grid gap-6 xl:grid-cols-[minmax(0,2fr)_minmax(0,1fr)]'>
        <Section title='运行状态' description='来自节点心跳，实时更新。'>
          <DescriptionList
            columns={3}
            items={[
              {
                label: '在线状态',
                value: <NodeConnectionChip runtime={runtime} />,
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
                label: '配置',
                value: (
                  <StatusChip
                    status={pending ? 'pending' : 'applied'}
                    label={pending ? '待生效' : '已生效'}
                    reserve={specStateLabels}
                  />
                ),
              },
              {
                label: '健康',
                value:
                  metrics.health &&
                  nodeHealthStatuses.includes(metrics.health) ? (
                    <StatusChip
                      status={metrics.health}
                      reserve={nodeHealthLabels}
                    />
                  ) : (
                    <TruncatedText
                      text={metrics.health}
                      className='max-w-full text-muted'
                    />
                  ),
              },
              {
                label: '排队日志',
                value: formatNumber(metrics.queued_log_events),
              },
              {
                label: '丢弃日志',
                value: formatNumber(metrics.dropped_log_events),
              },
              {
                label: '最近错误',
                wide: true,
                value: (
                  <TruncatedText
                    text={runtime.last_error}
                    className='max-w-full text-danger'
                  />
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
