import { useState } from 'react'
import {
  keepPreviousData,
  useMutation,
  useQuery,
  useQueryClient,
} from '@tanstack/react-query'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { formatBytesPerSecond, formatDate } from '@/lib/format'
import { queryKeys } from '@/lib/query-keys'
import { Confirm } from '@/components/confirm'
import { Choice, Field, Notice } from '@/components/forms'
import { Page, QueryNotice } from '@/components/page'
import { ResourceList } from '@/components/resource-list'
import { RowMenu } from '@/components/row-menu'
import { StatusChip } from '@/components/status-chip'
import { clusterOptionsQuery } from '@/features/clusters/data'
import { CredentialsDialog } from './credentials-dialog'
import {
  getNodeCredentials,
  nodesQuery,
  removeNode,
  type NodeCredentials,
} from './data'
import { NodeDialog } from './node-dialog'
import { NodeLogDialog } from './node-log-dialog'
import type { Node } from './types'

export function NodesPage() {
  const [createOpen, setCreateOpen] = useState(false)
  return (
    <Page
      title='节点管理'
      description='管理边缘节点、接入配置与实时运行状态。'
      actions={
        <Button size='sm' onPress={() => setCreateOpen(true)}>
          添加节点
        </Button>
      }
    >
      <NodesPanel createOpen={createOpen} onCreateOpenChange={setCreateOpen} />
    </Page>
  )
}

export function NodesPanel({
  initialClusterId,
  createOpen = false,
  onCreateOpenChange,
  showClusterFilter = true,
}: {
  initialClusterId?: string
  createOpen?: boolean
  onCreateOpenChange?: (open: boolean) => void
  showClusterFilter?: boolean
}) {
  const client = useQueryClient()
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(20)
  const [draft, setDraft] = useState('')
  const [keyword, setKeyword] = useState('')
  const [clusterId, setClusterId] = useState(initialClusterId ?? 'all')
  const [status, setStatus] = useState('all')
  const [dialog, setDialog] = useState<Node | null>(null)
  const [removeTarget, setRemoveTarget] = useState<Node | null>(null)
  const [credentials, setCredentials] = useState<NodeCredentials | null>(null)
  const [logNode, setLogNode] = useState<Node | null>(null)
  const clusters = useQuery(clusterOptionsQuery)
  const query = useQuery({
    ...nodesQuery({
      page,
      page_size: pageSize,
      keyword: keyword || undefined,
      cluster_id: clusterId === 'all' ? undefined : clusterId,
      status: status === 'all' ? undefined : status,
    }),
    placeholderData: keepPreviousData,
    enabled: !logNode,
  })
  const remove = useMutation({
    mutationFn: removeNode,
    onSuccess: (response) => {
      toast.success(response.message)
      setRemoveTarget(null)
      void client.invalidateQueries({ queryKey: queryKeys.nodes })
      void client.invalidateQueries({ queryKey: queryKeys.clusters })
    },
  })
  const reveal = useMutation({
    mutationFn: (node: Node) => getNodeCredentials(node.id),
    onSuccess: setCredentials,
  })
  const columns: DataGridColumn<Node>[] = [
    {
      id: 'name',
      header: '节点',
      isRowHeader: true,
      minWidth: 160,
      cell: (node) => (
        <div>
          <strong className='block text-sm font-medium'>{node.name}</strong>
          <span className='text-xs text-muted'>
            {node.runtime.agent_version || '等待接入'}
          </span>
        </div>
      ),
    },
    {
      id: 'cluster',
      header: '集群',
      minWidth: 130,
      cell: (node) => node.cluster_name,
    },
    {
      id: 'connection',
      header: '连接',
      minWidth: 100,
      cell: (node) => <StatusChip status={node.runtime.connection_status} />,
    },
    {
      id: 'endpoint',
      header: '服务端点',
      minWidth: 220,
      cell: (node) => (
        <div className='grid gap-1'>
          {node.config.endpoints.map((endpoint) => (
            <div key={endpoint.id} className='flex flex-wrap gap-x-3 gap-y-0.5'>
              <code className='text-xs'>{endpoint.ip_address}</code>
              <span className='text-xs text-muted'>{endpoint.line_code}</span>
            </div>
          ))}
        </div>
      ),
    },
    {
      id: 'metrics',
      header: '运行指标',
      minWidth: 235,
      cell: (node) => (
        <div className='grid gap-0.5 text-xs tabular-nums'>
          <span>
            CPU {node.runtime.cpu_usage?.toFixed(1) ?? '—'}% · 内存{' '}
            {node.runtime.memory_usage?.toFixed(1) ?? '—'}%
          </span>
          <span className='text-muted'>
            {formatBytesPerSecond(node.runtime.traffic_out_bps)} ·{' '}
            {node.runtime.connection_count?.toLocaleString() ?? '—'} 连接
          </span>
        </div>
      ),
    },
    {
      id: 'heartbeat',
      header: '最近心跳',
      minWidth: 175,
      cell: (node) => (
        <time
          className='text-xs text-muted tabular-nums'
          dateTime={node.runtime.last_heartbeat_at}
        >
          {formatDate(node.runtime.last_heartbeat_at)}
        </time>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 100,
      cell: (node) => <StatusChip status={node.status} />,
    },
    {
      id: 'actions',
      header: '操作',
      minWidth: 70,
      cell: (node) => (
        <RowMenu
          label={`管理节点 ${node.name}`}
          actions={[
            { id: 'edit', label: '编辑配置', onAction: () => setDialog(node) },
            {
              id: 'credentials',
              label: '接入凭据',
              onAction: () => reveal.mutate(node),
              disabled: reveal.isPending,
            },
            { id: 'logs', label: '实时日志', onAction: () => setLogNode(node) },
            {
              id: 'remove',
              label: '删除节点',
              onAction: () => {
                remove.reset()
                setRemoveTarget(node)
              },
              danger: true,
            },
          ]}
        />
      ),
    },
  ]
  return (
    <>
      <QueryNotice query={clusters} onRetry={() => void clusters.refetch()} />
      {reveal.isError && <Notice>{apiErrorMessage(reveal.error)}</Notice>}
      <ResourceList
        label='节点列表'
        query={query}
        data={query.data}
        columns={columns}
        page={page}
        onPageChange={setPage}
        pageSize={pageSize}
        onPageSizeChange={(value) => {
          setPageSize(value)
          setPage(1)
        }}
        onRefresh={() => void query.refetch()}
        emptyTitle='暂无边缘节点'
        emptyDescription='添加节点并使用接入凭据启动节点程序。'
        filters={
          <form
            className='flex flex-wrap items-end gap-2'
            onSubmit={(event) => {
              event.preventDefault()
              setKeyword(draft.trim())
              setPage(1)
              if (keyword === draft.trim() && page === 1) void query.refetch()
            }}
          >
            {showClusterFilter && (
              <Choice
                compact
                label='所属集群'
                value={clusterId}
                onChange={(value) => {
                  setClusterId(value)
                  setPage(1)
                }}
                items={[
                  { id: 'all', label: '全部集群' },
                  ...(clusters.data ?? []).map((cluster) => ({
                    id: cluster.id,
                    label: cluster.name,
                  })),
                ]}
              />
            )}
            <Choice
              compact
              label='节点状态'
              value={status}
              onChange={(value) => {
                setStatus(value)
                setPage(1)
              }}
              items={[
                { id: 'all', label: '全部状态' },
                { id: 'enabled', label: '已启用' },
                { id: 'disabled', label: '已停用' },
              ]}
            />
            <div className='w-52 max-w-full'>
              <Field
                label='搜索节点'
                value={draft}
                onChange={setDraft}
                placeholder='节点名称'
              />
            </div>
            <Button size='sm' variant='secondary' type='submit'>
              搜索
            </Button>
            {(keyword ||
              status !== 'all' ||
              (showClusterFilter && clusterId !== 'all')) && (
              <Button
                size='sm'
                variant='tertiary'
                onPress={() => {
                  setDraft('')
                  setKeyword('')
                  setStatus('all')
                  setClusterId(initialClusterId ?? 'all')
                  setPage(1)
                }}
              >
                重置
              </Button>
            )}
          </form>
        }
      />
      {(dialog || createOpen) && (
        <NodeDialog
          key={dialog?.id ?? 'new'}
          node={
            dialog
              ? (query.data?.list.find((node) => node.id === dialog.id) ??
                dialog)
              : undefined
          }
          clusters={clusters.data ?? []}
          initialClusterId={clusterId === 'all' ? undefined : clusterId}
          onCredentials={setCredentials}
          onClose={() => {
            setDialog(null)
            onCreateOpenChange?.(false)
          }}
        />
      )}
      {credentials && (
        <CredentialsDialog
          credentials={credentials}
          onClose={() => setCredentials(null)}
        />
      )}
      {logNode && (
        <NodeLogDialog node={logNode} onClose={() => setLogNode(null)} />
      )}
      {removeTarget && (
        <Confirm
          title='删除节点'
          description={`确定删除“${removeTarget.name}”吗？该节点将无法继续接入控制面。`}
          onClose={() => setRemoveTarget(null)}
          busy={remove.isPending}
          onConfirm={() => remove.mutateAsync(removeTarget)}
        />
      )}
    </>
  )
}
