import { useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { ListView } from '@heroui-pro/react'
import { KPI } from '@heroui-pro/react/kpi'
import { Alert, Skeleton } from '@heroui/react'
import { formatNumber } from '@/lib/format'
import { describeStatus } from '@/lib/status'
import { TruncatedText } from '@/components/data-table'
import { KpiQuantity } from '@/components/kpi-quantity'
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
  nodeHealthReserve,
  nodeHealthStatuses,
  nodeSpecStateReserve,
  nodeSpecStates,
} from './node-display'
import type { Node } from './types'

const route = getRouteApi('/_authenticated/nodes/$nodeId')

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
      <KPI.Content className='content-start'>
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

/** Count KPI; a missing value shows `—` in the same slot. */
function CountKpi({
  label,
  value,
  fractionDigits = 0,
}: {
  label: string
  value?: number
  fractionDigits?: number
}) {
  return (
    <KPI>
      <KPI.Header>
        <KPI.Title>{label}</KPI.Title>
      </KPI.Header>
      <KPI.Content className='content-start'>
        {value === undefined ? (
          <KPI.Value value={0}>{() => '—'}</KPI.Value>
        ) : (
          <KPI.Value
            value={value}
            minimumFractionDigits={fractionDigits}
            maximumFractionDigits={fractionDigits}
          />
        )}
      </KPI.Content>
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
    <ListView
      aria-label='服务端点'
      variant='secondary'
      items={node.config.endpoints}
    >
      {(endpoint) => (
        <ListView.Item
          id={endpoint.id}
          textValue={endpoint.ip_address}
          className='cursor-default px-0'
        >
          <ListView.ItemContent>
            <ListView.Title className='font-mono text-[13px] font-normal'>
              {endpoint.ip_address}
            </ListView.Title>
          </ListView.ItemContent>
          <ListView.ItemAction className='min-w-0 truncate text-[13px] text-muted'>
            {lines.data
              ? dnsLinePath(lines.data, endpoint.line_code)
              : endpoint.line_code === 'default'
                ? '默认线路'
                : endpoint.line_code}
          </ListView.ItemAction>
        </ListView.Item>
      )}
    </ListView>
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
              安装命令与接入凭据在“配置 → 接入凭据”中查看。
            </Alert.Description>
          </Alert.Content>
        </Alert>
      )}
      <div className='grid grid-cols-2 gap-3 md:grid-cols-3 xl:grid-cols-6'>
        <LoadKpi label='CPU' value={metrics.cpu_usage} />
        <LoadKpi label='内存' value={metrics.memory_usage} />
        <KPI>
          <KPI.Header>
            <KPI.Title>带宽</KPI.Title>
          </KPI.Header>
          <KPI.Content className='content-start'>
            <KpiQuantity value={metrics.traffic_out_bps} unit='bps' />
          </KPI.Content>
        </KPI>
        <CountKpi label='连接数' value={metrics.connection_count} />
        <CountKpi label='连接 IP' value={metrics.connected_ip_count} />
        <CountKpi
          label='1 分钟负载'
          value={metrics.load_1m}
          fractionDigits={2}
        />
      </div>
      <div className='grid gap-6 xl:grid-cols-[minmax(0,2fr)_minmax(0,1fr)]'>
        <Section title='运行状态'>
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
                    {...nodeSpecStates[pending ? 'pending' : 'applied']}
                    reserve={nodeSpecStateReserve}
                  />
                ),
              },
              {
                label: '健康',
                value:
                  metrics.health &&
                  Object.hasOwn(nodeHealthStatuses, metrics.health) ? (
                    <StatusChip
                      {...describeStatus(nodeHealthStatuses, metrics.health)}
                      reserve={nodeHealthReserve}
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
        <Section title='服务端点'>
          <Endpoints node={node} />
        </Section>
      </div>
    </div>
  )
}
