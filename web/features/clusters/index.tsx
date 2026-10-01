import { useState } from 'react'
import {
  keepPreviousData,
  useMutation,
  useQuery,
  useQueryClient,
} from '@tanstack/react-query'
import { useNavigate, useSearch } from '@tanstack/react-router'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, toast } from '@heroui/react'
import { queryKeys } from '@/lib/query-keys'
import { Confirm } from '@/components/confirm'
import { Choice, Field } from '@/components/forms'
import { Page, QueryNotice } from '@/components/page'
import { ResourceList } from '@/components/resource-list'
import { RowMenu } from '@/components/row-menu'
import { StatusChip } from '@/components/status-chip'
import { dnsZoneOptionsQuery } from '@/features/dns-zones/data'
import { NodesPanel } from '@/features/nodes'
import { ClusterDialog } from './cluster-dialog'
import { clusterOptionsQuery, clustersQuery, removeCluster } from './data'
import type { Cluster } from './types'

export function ClustersPage() {
  const client = useQueryClient()
  const navigate = useNavigate()
  const search = useSearch({ strict: false }) as Record<string, unknown>
  const selectedId =
    typeof search.cluster_id === 'string' ? search.cluster_id : undefined
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(20)
  const [draft, setDraft] = useState('')
  const [keyword, setKeyword] = useState('')
  const [status, setStatus] = useState('all')
  const [zoneId, setZoneId] = useState('all')
  const [dialog, setDialog] = useState<Cluster | 'new' | null>(null)
  const [removeTarget, setRemoveTarget] = useState<Cluster | null>(null)
  const [createNodeOpen, setCreateNodeOpen] = useState(false)
  const query = useQuery({
    ...clustersQuery({
      page,
      page_size: pageSize,
      keyword: keyword || undefined,
      status: status === 'all' ? undefined : status,
      dns_zone_id: zoneId === 'all' ? undefined : zoneId,
    }),
    placeholderData: keepPreviousData,
    enabled: !selectedId,
  })
  const options = useQuery({ ...clusterOptionsQuery, enabled: !!selectedId })
  const zones = useQuery({ ...dnsZoneOptionsQuery, enabled: !selectedId })
  const selected =
    options.data?.find((cluster) => cluster.id === selectedId) ??
    query.data?.list.find((cluster) => cluster.id === selectedId)
  function selectCluster(id?: string) {
    setCreateNodeOpen(false)
    void navigate({
      to: '.',
      search: (previous: Record<string, unknown>) => ({
        ...previous,
        cluster_id: id,
        view: id ? 'nodes' : undefined,
      }),
      replace: true,
    })
  }
  const remove = useMutation({
    mutationFn: removeCluster,
    onSuccess: (response) => {
      toast.success(response.message)
      setRemoveTarget(null)
      void client.invalidateQueries({ queryKey: queryKeys.clusters })
    },
  })
  const columns: DataGridColumn<Cluster>[] = [
    {
      id: 'name',
      header: '集群',
      isRowHeader: true,
      minWidth: 190,
      cell: (cluster) => (
        <Button
          size='sm'
          variant='ghost'
          className='h-auto justify-start px-0 py-0 font-medium'
          onPress={() => selectCluster(cluster.id)}
        >
          {cluster.name}
        </Button>
      ),
    },
    {
      id: 'domain',
      header: '接入域名',
      minWidth: 220,
      cell: (cluster) => (
        <code className='text-xs'>{cluster.access_domain}</code>
      ),
    },
    {
      id: 'dns',
      header: '托管域名',
      minWidth: 200,
      cell: (cluster) => (
        <div>
          <span className='block text-sm'>{cluster.dns_zone_domain}</span>
          <small className='text-xs text-muted'>
            {cluster.dns_provider_name}
          </small>
        </div>
      ),
    },
    {
      id: 'nodes',
      header: '边缘节点',
      minWidth: 130,
      cell: (cluster) => (
        <span className='tabular-nums'>
          {cluster.online_node_count} / {cluster.node_count}
          <span className='ml-1 text-xs text-muted'>在线</span>
        </span>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 100,
      cell: (cluster) => <StatusChip status={cluster.status} />,
    },
    {
      id: 'actions',
      header: '操作',
      minWidth: 70,
      cell: (cluster) => (
        <RowMenu
          label={`管理集群 ${cluster.name}`}
          actions={[
            {
              id: 'nodes',
              label: '查看节点',
              onAction: () => selectCluster(cluster.id),
            },
            {
              id: 'edit',
              label: '编辑集群',
              onAction: () => setDialog(cluster),
            },
            {
              id: 'remove',
              label: '删除集群',
              danger: true,
              onAction: () => {
                remove.reset()
                setRemoveTarget(cluster)
              },
            },
          ]}
        />
      ),
    },
  ]
  return (
    <Page
      title='集群管理'
      description={
        selectedId
          ? '查看集群的接入域名、节点配置与实时运行状态。'
          : '按集群组织边缘节点，配置 DNS 接入域名。'
      }
      actions={
        selectedId ? (
          <>
            <Button
              size='sm'
              variant='tertiary'
              onPress={() => selectCluster()}
            >
              全部集群
            </Button>
            {selected && (
              <Button
                size='sm'
                variant='secondary'
                onPress={() => setDialog(selected)}
              >
                编辑集群
              </Button>
            )}
            <Button size='sm' onPress={() => setCreateNodeOpen(true)}>
              添加节点
            </Button>
          </>
        ) : (
          <Button size='sm' onPress={() => setDialog('new')}>
            创建集群
          </Button>
        )
      }
    >
      {selectedId ? (
        <>
          <QueryNotice query={options} onRetry={() => void options.refetch()} />
          {selected && (
            <div className='grid gap-3 rounded-xl bg-surface-secondary p-4 sm:grid-cols-3'>
              <div>
                <p className='text-sm font-medium'>{selected.name}</p>
                <div className='mt-2'>
                  <StatusChip status={selected.status} />
                </div>
              </div>
              <div>
                <p className='text-xs text-muted'>接入域名</p>
                <code className='mt-1 block text-sm wrap-anywhere'>
                  {selected.access_domain}
                </code>
                <p className='mt-1 text-xs text-muted'>
                  {selected.dns_zone_domain} · {selected.dns_provider_name}
                </p>
              </div>
              <div>
                <p className='text-xs text-muted'>边缘节点</p>
                <p className='mt-1 text-lg font-medium tabular-nums'>
                  {selected.online_node_count} / {selected.node_count}
                  <span className='ml-2 text-xs font-normal text-muted'>
                    在线
                  </span>
                </p>
              </div>
            </div>
          )}
          <NodesPanel
            key={selectedId}
            initialClusterId={selectedId}
            showClusterFilter={false}
            createOpen={createNodeOpen}
            onCreateOpenChange={setCreateNodeOpen}
          />
        </>
      ) : (
        <>
          <QueryNotice query={zones} onRetry={() => void zones.refetch()} />
          <ResourceList
            label='集群列表'
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
            emptyTitle='暂无集群'
            emptyDescription='创建集群后即可配置边缘节点和接入域名。'
            filters={
              <form
                className='flex flex-wrap items-end gap-2'
                onSubmit={(event) => {
                  event.preventDefault()
                  setKeyword(draft.trim())
                  setPage(1)
                  if (draft.trim() === keyword && page === 1)
                    void query.refetch()
                }}
              >
                <Choice
                  compact
                  label='启用状态'
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
                <Choice
                  compact
                  label='托管域名'
                  value={zoneId}
                  onChange={(value) => {
                    setZoneId(value)
                    setPage(1)
                  }}
                  items={[
                    { id: 'all', label: '全部域名' },
                    ...(zones.data ?? []).map((zone) => ({
                      id: zone.id,
                      label: zone.domain,
                    })),
                  ]}
                />
                <div className='w-52 max-w-full'>
                  <Field
                    label='搜索集群'
                    value={draft}
                    onChange={setDraft}
                    placeholder='集群名称'
                  />
                </div>
                <Button size='sm' variant='secondary' type='submit'>
                  搜索
                </Button>
                {(keyword || status !== 'all' || zoneId !== 'all') && (
                  <Button
                    size='sm'
                    variant='tertiary'
                    onPress={() => {
                      setDraft('')
                      setKeyword('')
                      setStatus('all')
                      setZoneId('all')
                      setPage(1)
                    }}
                  >
                    重置
                  </Button>
                )}
              </form>
            }
          />
        </>
      )}
      {dialog && (
        <ClusterDialog
          key={dialog === 'new' ? 'new' : dialog.id}
          cluster={dialog === 'new' ? undefined : dialog}
          onClose={() => setDialog(null)}
        />
      )}
      {removeTarget && (
        <Confirm
          title='删除集群'
          description={`确定删除“${removeTarget.name}”吗？存在关联节点时无法删除集群。`}
          busy={remove.isPending}
          onConfirm={() => remove.mutateAsync(removeTarget)}
          onClose={() => setRemoveTarget(null)}
        />
      )}
    </Page>
  )
}
