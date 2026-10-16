import type { DataGridColumn } from '@heroui-pro/react'
import { formatDate } from '@/lib/format'
import { describeStatus } from '@/lib/status'
import { DataTable } from '@/components/data-table'
import { Mono } from '@/components/mono'
import { Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { type DnsLineTreeNode, dnsLineIndex } from './dns-lines'
import { lineStatuses, lineStatusReserve } from './dns-zone-display'
import { useZoneDetail } from './use-zone-detail'

const columns: DataGridColumn<DnsLineTreeNode>[] = [
  {
    id: 'name',
    header: '线路',
    isRowHeader: true,
    minWidth: 240,
    cell: ({ label }) => <span>{label}</span>,
  },
  {
    id: 'code',
    header: '线路代码',
    minWidth: 160,
    cell: ({ line }) =>
      line ? <Mono>{line.code}</Mono> : <span className='text-muted'>—</span>,
  },
  {
    id: 'status',
    header: '状态',
    minWidth: 110,
    cell: ({ line }) =>
      line ? (
        <StatusChip
          {...describeStatus(lineStatuses, line.status)}
          reserve={lineStatusReserve}
        />
      ) : (
        <span className='text-muted'>—</span>
      ),
  },
]

/** Expandable provider hierarchy from the zone snapshot's shared index.
 * Group rows remain with their children instead of splitting across pages. */
export function ZoneLines() {
  const zone = useZoneDetail().data
  if (!zone) return null
  const { tree, options } = dnsLineIndex(zone.runtime.lines)
  return (
    <Section
      title={`解析线路 · ${options.length}`}
      description={`服务商支持的解析线路，记录和节点服务端点按线路分别解析。最近更新：${formatDate(zone.last_synced_at)}`}
    >
      <DataTable
        label='解析线路'
        variant='secondary'
        columns={columns}
        data={tree}
        isLoading={false}
        getRowId={({ key }) => key}
        getChildren={({ children }) => children}
        minWidth={560}
        empty={{
          title: '暂无解析线路',
          description: '执行一次同步后会从服务商读取可用线路。',
        }}
      />
    </Section>
  )
}
