import { useState } from 'react'
import {
  keepPreviousData,
  useMutation,
  useQuery,
  useQueryClient,
} from '@tanstack/react-query'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Chip, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { Confirm } from '@/components/confirm'
import { Choice, Field } from '@/components/forms'
import { Page } from '@/components/page'
import { ResourceList } from '@/components/resource-list'
import { RowMenu } from '@/components/row-menu'
import { StatusChip } from '@/components/status-chip'
import { clusterOptionsQuery } from '@/features/clusters/data'
import { AccessLogs } from './access-logs'
import { websitesQuery, removeWebsite } from './data'
import type { Website } from './types'
import { WebsiteCacheOperations } from './website-cache-operations'
import { WebsiteDetail } from './website-detail'
import { WebsiteDialog } from './website-dialog'

type View =
  | { kind: 'edit' | 'detail' | 'logs' | 'cache'; website: Website }
  | { kind: 'new' }

export function WebsitesPage() {
  const client = useQueryClient()
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(20)
  const [keyword, setKeyword] = useState('')
  const [search, setSearch] = useState('')
  const [cluster, setCluster] = useState('all')
  const [status, setStatus] = useState('all')
  const [view, setView] = useState<View | null>(null)
  const [removing, setRemoving] = useState<Website | null>(null)
  const clusters = useQuery({ ...clusterOptionsQuery, enabled: !view })
  const query = useQuery({
    ...websitesQuery({
      page,
      page_size: pageSize,
      keyword: search || undefined,
      cluster_id: cluster === 'all' ? undefined : cluster,
      status: status === 'all' ? undefined : status,
    }),
    enabled: !view,
    placeholderData: keepPreviousData,
  })
  const remove = useMutation({
    mutationFn: removeWebsite,
    onSuccess: () => {
      toast.success('网站已删除')
      setRemoving(null)
      void client.invalidateQueries({ queryKey: queryKeys.websites })
    },
    onError: (error) => toast.danger(apiErrorMessage(error)),
  })
  const columns: DataGridColumn<Website>[] = [
    {
      id: 'name',
      header: '网站',
      isRowHeader: true,
      minWidth: 210,
      cell: (website) => (
        <div className='flex flex-col gap-1'>
          <Button
            variant='ghost'
            size='sm'
            className='h-auto justify-start p-0 font-medium'
            onPress={() => setView({ kind: 'detail', website })}
          >
            {website.config.name ||
              website.config.domains[0]?.hostname ||
              '未命名网站'}
          </Button>
          <span
            className='max-w-72 truncate text-xs text-muted'
            title={website.config.domains
              .map((domain) => domain.hostname)
              .join('、')}
          >
            {website.config.domains.map((domain) => domain.hostname).join('、')}
          </span>
        </div>
      ),
    },
    {
      id: 'cluster',
      header: '所属集群',
      minWidth: 180,
      cell: (website) => (
        <div className='flex flex-col gap-1'>
          <span>{website.cluster_name}</span>
          <code className='text-xs text-muted'>{website.access_domain}</code>
        </div>
      ),
    },
    {
      id: 'origin',
      header: '默认源站',
      minWidth: 190,
      cell: (website) => {
        const origin = website.config.origins.find(
          (item) =>
            item.group === website.config.default_origin_group &&
            item.role === 'primary'
        )
        return origin ? (
          <div className='flex flex-col gap-1'>
            <span>{origin.group}</span>
            <code className='text-xs text-muted'>
              {origin.protocol}://{origin.host}:{origin.port}
            </code>
          </div>
        ) : (
          '未配置'
        )
      },
    },
    {
      id: 'deployment',
      header: '节点发布',
      minWidth: 130,
      cell: (website) => (
        <div className='flex flex-col items-start gap-1'>
          <StatusChip status={website.runtime.deploy_status} />
          <span className='text-xs text-muted tabular-nums'>
            {website.runtime.synced_node_count} /{' '}
            {website.runtime.target_node_count} 节点
          </span>
        </div>
      ),
    },
    {
      id: 'tls',
      header: 'HTTPS',
      minWidth: 110,
      cell: (website) => (
        <Chip
          size='sm'
          variant='soft'
          color={website.config.https_enabled ? 'accent' : 'default'}
        >
          {website.config.https_enabled
            ? `TLS ${website.config.minimum_tls_version}+`
            : '未启用'}
        </Chip>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 100,
      cell: (website) => <StatusChip status={website.status} />,
    },
    {
      id: 'actions',
      header: '操作',
      width: 70,
      cell: (website) => (
        <RowMenu
          label={`${website.config.name || website.access_domain} 操作`}
          actions={[
            {
              id: 'detail',
              label: '查看详情',
              onAction: () => setView({ kind: 'detail', website }),
            },
            {
              id: 'edit',
              label: '编辑配置',
              onAction: () => setView({ kind: 'edit', website }),
            },
            {
              id: 'logs',
              label: '访问日志',
              onAction: () => setView({ kind: 'logs', website }),
            },
            {
              id: 'cache',
              label: '缓存刷新与预热',
              onAction: () => setView({ kind: 'cache', website }),
            },
            {
              id: 'delete',
              label: '删除网站',
              danger: true,
              onAction: () => setRemoving(website),
            },
          ]}
        />
      ),
    },
  ]
  return (
    <Page
      title='网站'
      description='管理域名、源站与边缘分发配置。'
      actions={
        <Button size='sm' onPress={() => setView({ kind: 'new' })}>
          创建网站
        </Button>
      }
    >
      <ResourceList
        label='网站列表'
        query={query}
        columns={columns}
        page={page}
        onPageChange={setPage}
        pageSize={pageSize}
        onPageSizeChange={(size) => {
          setPageSize(size)
          setPage(1)
        }}
        onRefresh={() => void query.refetch()}
        emptyTitle='暂无网站'
        emptyDescription='创建网站，连接域名与源站。'
        filters={
          <form
            className='flex flex-wrap items-end gap-2'
            onSubmit={(event) => {
              event.preventDefault()
              setSearch(keyword.trim())
              setPage(1)
            }}
          >
            <div className='min-w-52 flex-1'>
              <Field
                label='搜索网站'
                value={keyword}
                onChange={setKeyword}
                placeholder='网站名称或域名'
              />
            </div>
            <Choice
              compact
              label='所属集群'
              value={cluster}
              onChange={(value) => {
                setCluster(value)
                setPage(1)
              }}
              items={[
                { id: 'all', label: '全部集群' },
                ...(clusters.data ?? []).map((item) => ({
                  id: item.id,
                  label: item.name,
                })),
              ]}
            />
            <Choice
              compact
              label='网站状态'
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
            <Button size='sm' variant='secondary' type='submit'>
              搜索
            </Button>
          </form>
        }
      />
      {view?.kind === 'new' && <WebsiteDialog onClose={() => setView(null)} />}
      {view?.kind === 'edit' && (
        <WebsiteDialog website={view.website} onClose={() => setView(null)} />
      )}
      {view?.kind === 'detail' && (
        <WebsiteDetail
          website={view.website}
          onClose={() => setView(null)}
          onEdit={(website) => setView({ kind: 'edit', website })}
        />
      )}
      {view?.kind === 'logs' && (
        <AccessLogs website={view.website} onClose={() => setView(null)} />
      )}
      {view?.kind === 'cache' && (
        <WebsiteCacheOperations
          website={view.website}
          onClose={() => setView(null)}
        />
      )}
      {removing && (
        <Confirm
          title='删除网站'
          description={`删除「${removing.config.name || removing.access_domain}」及其节点配置。此操作无法撤销。`}
          onClose={() => setRemoving(null)}
          busy={remove.isPending}
          onConfirm={() => remove.mutateAsync(removing)}
        />
      )}
    </Page>
  )
}
