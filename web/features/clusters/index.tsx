import { useState } from 'react'
import { useMutation, useQuery } from '@tanstack/react-query'
import { useNavigate, useSearch } from '@tanstack/react-router'
import { FolderTree, Pencil, Plus, Server, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import { cn } from '@/lib/utils'
import { useDelayedLoading } from '@/hooks/use-delayed-loading'
import { Button } from '@/components/ui/button'
import {
  DropdownMenuItem,
  DropdownMenuSeparator,
} from '@/components/ui/dropdown-menu'
import { Skeleton } from '@/components/ui/skeleton'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { FeatureShell } from '@/components/feature-shell'
import { RowActions } from '@/components/row-actions'
import { StatusBadge } from '@/components/status-badge'
import type { Cluster } from '@/features/clusters/types'
import { NodesPanel } from '@/features/nodes'
import { ClusterDialog } from './cluster-dialog'
import { clusterOptionsQuery, removeCluster } from './data'

export function Clusters() {
  const navigate = useNavigate()
  const search = useSearch({ from: '/_authenticated/clusters' })
  const [dialog, setDialog] = useState<Cluster | 'new' | null>(null)
  const [removeTarget, setRemoveTarget] = useState<Cluster | null>(null)
  const [createNodeOpen, setCreateNodeOpen] = useState(false)
  const clustersQuery = useQuery(clusterOptionsQuery)
  const [visibleCount, setVisibleCount] = useState(100)
  const clustersLoading = useDelayedLoading(clustersQuery.isLoading)
  const allClusters = clustersQuery.data ?? []
  const clusters = allClusters.slice(0, visibleCount)
  const selected = clusters.find((cluster) => cluster.id === search.cluster_id)
  const selectCluster = (id?: string) => {
    setCreateNodeOpen(false)
    void navigate({
      to: '/clusters',
      search: { view: 'nodes', cluster_id: id },
    })
  }
  const remove = useMutation({
    mutationFn: removeCluster,
    onSuccess: (response, cluster) => {
      toast.success(response.message)
      setRemoveTarget(null)
      if (search.cluster_id === cluster.id) selectCluster()
    },
  })
  return (
    <FeatureShell
      fixed
      title='集群管理'
      description='从左侧选择集群，管理该集群的节点与运行状态。'
    >
      <div className='grid min-h-0 flex-1 gap-4 overflow-y-auto md:grid-cols-[17rem_minmax(0,1fr)] md:overflow-hidden'>
        <aside
          aria-label='集群导航'
          className='flex min-h-0 flex-col rounded-md border md:overflow-hidden'
        >
          <div className='flex shrink-0 items-center justify-between border-b p-3'>
            <h2 className='flex items-center gap-2 text-sm font-semibold'>
              <FolderTree className='size-4' />
              集群
            </h2>
            <Button size='sm' onClick={() => setDialog('new')}>
              <Plus />
              创建集群
            </Button>
          </div>
          <nav
            aria-label='集群列表'
            className='max-h-64 min-h-0 overflow-y-auto p-2 md:max-h-none md:flex-1'
          >
            <div className='space-y-1'>
              {clustersLoading.pending && (
                <div
                  className={cn(
                    'space-y-1',
                    !clustersLoading.showSkeleton && 'invisible'
                  )}
                >
                  <Skeleton className='h-9' />
                  <Skeleton className='h-9' />
                </div>
              )}
              {!clustersLoading.pending &&
                clusters.map((cluster) => (
                  <div
                    key={cluster.id}
                    className={cn(
                      'flex items-center rounded-md',
                      search.cluster_id === cluster.id && 'bg-secondary'
                    )}
                  >
                    <Button
                      variant='ghost'
                      className='min-w-0 flex-1 justify-start px-2'
                      aria-current={
                        search.cluster_id === cluster.id ? 'page' : undefined
                      }
                      onClick={() => selectCluster(cluster.id)}
                      title={cluster.name + ' · ' + cluster.access_domain}
                    >
                      <Server className='size-4 shrink-0' />
                      <span className='truncate'>{cluster.name}</span>
                      <span className='ms-auto text-xs text-muted-foreground'>
                        {cluster.online_node_count}/{cluster.node_count}
                      </span>
                    </Button>
                    <RowActions>
                      <DropdownMenuItem onSelect={() => setDialog(cluster)}>
                        <Pencil />
                        编辑集群
                      </DropdownMenuItem>
                      <DropdownMenuSeparator />
                      <DropdownMenuItem
                        variant='destructive'
                        onSelect={() => setRemoveTarget(cluster)}
                      >
                        <Trash2 />
                        删除集群
                      </DropdownMenuItem>
                    </RowActions>
                  </div>
                ))}
              {!clustersLoading.pending &&
                !clustersQuery.isError &&
                !clusters.length && (
                  <p className='p-2 text-sm text-muted-foreground'>暂无集群</p>
                )}
              {!clustersLoading.pending && clustersQuery.isError && (
                <div role='alert' className='space-y-2 p-2 text-sm'>
                  <p>集群加载失败</p>
                  <Button
                    size='sm'
                    variant='outline'
                    onClick={() => clustersQuery.refetch()}
                  >
                    重新加载
                  </Button>
                </div>
              )}
              {visibleCount < allClusters.length && (
                <Button
                  variant='ghost'
                  className='w-full'
                  disabled={clustersQuery.isFetching}
                  onClick={() => setVisibleCount((count) => count + 100)}
                >
                  加载更多集群
                </Button>
              )}
            </div>
          </nav>
        </aside>
        <section
          aria-label='节点列表'
          className='min-h-0 min-w-0 space-y-3 md:overflow-auto'
        >
          <div className='flex min-h-10 flex-wrap items-center justify-between gap-3'>
            <div className='flex min-w-0 items-center gap-2'>
              <h2 className='truncate text-base leading-none font-semibold'>
                {search.cluster_id
                  ? (selected?.name ?? '所选集群')
                  : '全部节点'}
              </h2>
              {selected && <StatusBadge status={selected.status} />}
            </div>
            <Button size='sm' onClick={() => setCreateNodeOpen(true)}>
              <Plus />
              添加节点
            </Button>
          </div>
          {selected && (
            <div className='flex flex-wrap items-center gap-x-5 gap-y-1 text-xs'>
              <div className='flex min-w-0 items-baseline gap-2'>
                <span className='shrink-0 text-muted-foreground'>托管域名</span>
                <span
                  className='truncate font-medium'
                  title={selected.dns_zone_domain}
                >
                  {selected.dns_zone_domain}
                </span>
                <span className='truncate text-muted-foreground'>
                  {selected.dns_provider_name}
                </span>
              </div>
              <div className='flex min-w-0 items-baseline gap-2'>
                <span className='shrink-0 text-muted-foreground'>接入域名</span>
                <code className='truncate' title={selected.access_domain}>
                  {selected.access_domain}
                </code>
              </div>
              <div className='flex items-baseline gap-2 whitespace-nowrap'>
                <span className='text-muted-foreground'>节点状态</span>
                <span className='font-medium tabular-nums'>
                  {selected.online_node_count}/{selected.node_count} 在线
                </span>
              </div>
            </div>
          )}
          <NodesPanel
            key={search.cluster_id ?? 'all'}
            initialClusterId={search.cluster_id}
            showClusterFilter={false}
            createOpen={createNodeOpen}
            onCreateOpenChange={setCreateNodeOpen}
          />
        </section>
      </div>
      {dialog && (
        <ClusterDialog
          key={dialog === 'new' ? 'new' : dialog.id}
          cluster={dialog === 'new' ? undefined : dialog}
          open
          onOpenChange={(open) => !open && setDialog(null)}
        />
      )}
      <ConfirmDialog
        open={!!removeTarget}
        onOpenChange={(open) => !open && setRemoveTarget(null)}
        title='删除集群'
        desc={
          '确定删除“' +
          (removeTarget?.name ?? '') +
          '”吗？有关联节点时服务端会拒绝删除。'
        }
        confirmText='确认删除'
        destructive
        isLoading={remove.isPending}
        handleConfirm={() => removeTarget && remove.mutate(removeTarget)}
      />
    </FeatureShell>
  )
}
