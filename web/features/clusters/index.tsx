import { useState } from 'react'
import {
  useInfiniteQuery,
  useMutation,
  useQueryClient,
} from '@tanstack/react-query'
import { useNavigate, useSearch } from '@tanstack/react-router'
import { FolderTree, Pencil, Plus, Server, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import { getData, sendData } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import type { Cluster, PageData } from '@/lib/types'
import { cn } from '@/lib/utils'
import { Button } from '@/components/ui/button'
import { Card, CardContent } from '@/components/ui/card'
import {
  DropdownMenuItem,
  DropdownMenuSeparator,
} from '@/components/ui/dropdown-menu'
import { Skeleton } from '@/components/ui/skeleton'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { FeatureShell } from '@/components/feature-shell'
import { RowActions } from '@/components/row-actions'
import { StatusBadge } from '@/components/status-badge'
import { NodesPanel } from '@/features/nodes'
import { ClusterDialog } from './cluster-dialog'

export function Clusters() {
  const navigate = useNavigate()
  const search = useSearch({ from: '/_authenticated/clusters' })
  const queryClient = useQueryClient()
  const [dialog, setDialog] = useState<Cluster | 'new' | null>(null)
  const [removeTarget, setRemoveTarget] = useState<Cluster | null>(null)
  const [createNodeOpen, setCreateNodeOpen] = useState(false)
  const clustersQuery = useInfiniteQuery({
    queryKey: [...queryKeys.clusters, 'tree'],
    initialPageParam: 1,
    queryFn: ({ pageParam }) =>
      getData<PageData<Cluster>>('/clusters/', {
        page: pageParam,
        page_size: 100,
      }),
    getNextPageParam: (last) =>
      last.page < last.total_pages ? last.page + 1 : undefined,
  })
  const clusters = clustersQuery.data?.pages.flatMap((page) => page.list) ?? []
  const selected = clusters.find((cluster) => cluster.id === search.cluster_id)
  const selectCluster = (id?: string) => {
    setCreateNodeOpen(false)
    void navigate({
      to: '/clusters',
      search: { view: 'nodes', cluster_id: id },
    })
  }
  const remove = useMutation({
    mutationFn: (cluster: Cluster) =>
      sendData(
        'delete',
        '/clusters/' + cluster.id,
        undefined,
        cluster.revision
      ),
    onSuccess: async (response, cluster) => {
      toast.success(response.message)
      setRemoveTarget(null)
      if (search.cluster_id === cluster.id) selectCluster()
      await queryClient.invalidateQueries({ queryKey: queryKeys.clusters })
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
              {clustersQuery.isLoading && (
                <>
                  <Skeleton className='h-9' />
                  <Skeleton className='h-9' />
                </>
              )}
              {clusters.map((cluster) => (
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
              {!clustersQuery.isLoading &&
                !clustersQuery.isError &&
                !clusters.length && (
                  <p className='p-2 text-sm text-muted-foreground'>暂无集群</p>
                )}
              {clustersQuery.isError && (
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
              {clustersQuery.hasNextPage && (
                <Button
                  variant='ghost'
                  className='w-full'
                  disabled={clustersQuery.isFetchingNextPage}
                  onClick={() => clustersQuery.fetchNextPage()}
                >
                  {clustersQuery.isFetchingNextPage
                    ? '正在加载…'
                    : '加载更多集群'}
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
              <h2 className='truncate text-base font-semibold leading-none'>
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
            <div className='grid gap-3 sm:grid-cols-2 lg:grid-cols-4'>
              <Card className='min-h-20 py-0 shadow-none'>
                <CardContent className='flex h-full flex-col justify-center px-4 py-3 text-sm'>
                  <div className='text-xs text-muted-foreground'>托管域名</div>
                  <div
                    className='truncate font-medium'
                    title={`${selected.dns_zone_domain} · ${selected.dns_provider_name}`}
                  >
                    {selected.dns_zone_domain}{' '}
                    <span className='font-normal text-muted-foreground'>
                      · {selected.dns_provider_name}
                    </span>
                  </div>
                </CardContent>
              </Card>
              <Card className='min-h-20 py-0 shadow-none'>
                <CardContent className='flex h-full flex-col justify-center px-4 py-3 text-sm'>
                  <div className='text-xs text-muted-foreground'>主机前缀</div>
                  <code className='block truncate'>
                    {selected.hostname_prefix}
                  </code>
                </CardContent>
              </Card>
              <Card className='min-h-20 py-0 shadow-none'>
                <CardContent className='flex h-full flex-col justify-center px-4 py-3 text-sm'>
                  <div className='text-xs text-muted-foreground'>接入域名</div>
                  <code
                    className='block truncate'
                    title={selected.access_domain}
                  >
                    {selected.access_domain}
                  </code>
                </CardContent>
              </Card>
              <Card className='min-h-20 py-0 shadow-none'>
                <CardContent className='flex h-full flex-col justify-center px-4 py-3 text-sm'>
                  <div className='text-xs text-muted-foreground'>节点状态</div>
                  <div className='font-medium'>
                    {selected.online_node_count}/{selected.node_count} 在线
                  </div>
                </CardContent>
              </Card>
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
