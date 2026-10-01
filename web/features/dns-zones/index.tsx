import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { formatDate } from '@/lib/format'
import { queryKeys } from '@/lib/query-keys'
import { Confirm } from '@/components/confirm'
import { Choice } from '@/components/forms'
import { Page, QueryNotice } from '@/components/page'
import { ResourceList, ResourceSearch } from '@/components/resource-list'
import { RowMenu } from '@/components/row-menu'
import { StatusChip } from '@/components/status-chip'
import { dnsProviderOptionsQuery } from '@/features/providers/data'
import { CreateZoneDialog } from './create-zone-dialog'
import { dnsZonesQuery, removeDnsZone, syncDnsZone } from './data'
import { displaySyncStatus, hasMeaningfulConflicts } from './dns-zone-display'
import { RecordsDialog } from './records-dialog'
import type { DnsZone } from './types'
import { ZoneDetailDialog } from './zone-detail'

export function DnsZonesPage() {
  const client = useQueryClient()
  const [page, setPage] = useState(1)
  const [size, setSize] = useState(20)
  const [draft, setDraft] = useState('')
  const [keyword, setKeyword] = useState('')
  const [provider, setProvider] = useState('all')
  const [create, setCreate] = useState(false)
  const [edit, setEdit] = useState<DnsZone | null>(null)
  const [detail, setDetail] = useState<DnsZone | null>(null)
  const [target, setTarget] = useState<DnsZone | null>(null)
  const [resolve, setResolve] = useState<{
    item: DnsZone
    policy: 'local' | 'remote'
  } | null>(null)
  const providers = useQuery(dnsProviderOptionsQuery)
  const query = useQuery(
    dnsZonesQuery({
      page,
      page_size: size,
      keyword: keyword || undefined,
      dns_provider_id: provider === 'all' ? undefined : provider,
    })
  )
  const sync = useMutation({
    mutationFn: syncDnsZone,
    onSuccess: (response) => {
      toast.success(response.message || '同步任务已提交')
      setResolve(null)
      void client.invalidateQueries({ queryKey: queryKeys.dnsZones })
    },
    onError: (error) => toast.danger(apiErrorMessage(error)),
  })
  const remove = useMutation({
    mutationFn: removeDnsZone,
    onSuccess: (response) => {
      toast.success(response.message || '已移除')
      setTarget(null)
      void client.invalidateQueries({ queryKey: queryKeys.dnsZones })
    },
    onError: (error) => toast.danger(apiErrorMessage(error)),
  })
  const columns: DataGridColumn<DnsZone>[] = [
    {
      id: 'domain',
      header: '托管域名',
      isRowHeader: true,
      minWidth: 220,
      cell: (item) => (
        <Button
          variant='ghost'
          size='sm'
          className='justify-start px-0 font-medium'
          onPress={() => setDetail(item)}
        >
          {item.domain}
        </Button>
      ),
    },
    {
      id: 'provider',
      header: 'DNS 账号',
      minWidth: 160,
      accessorKey: 'dns_provider_name',
    },
    {
      id: 'records',
      header: '记录',
      minWidth: 80,
      cell: (item) => (
        <span className='tabular-nums'>
          {item.config.records.length + item.runtime.projected_records.length}{' '}
          条
        </span>
      ),
    },
    {
      id: 'websites',
      header: '关联网站',
      minWidth: 100,
      cell: (item) => (
        <span className='tabular-nums'>{item.website_count} 个</span>
      ),
    },
    {
      id: 'status',
      header: '同步状态',
      minWidth: 120,
      cell: (item) => <StatusChip status={displaySyncStatus(item)} />,
    },
    {
      id: 'time',
      header: '最近同步',
      minWidth: 170,
      cell: (item) => (
        <span className='text-xs text-muted'>
          {formatDate(item.last_synced_at)}
        </span>
      ),
    },
    {
      id: 'actions',
      header: '操作',
      width: 70,
      cell: (item) => (
        <RowMenu
          label={`${item.domain}操作`}
          actions={[
            {
              id: 'detail',
              label: '查看详情',
              onAction: () => setDetail(item),
            },
            { id: 'records', label: '编辑记录', onAction: () => setEdit(item) },
            {
              id: 'sync',
              label: '立即同步',
              disabled: sync.isPending,
              onAction: () => sync.mutate({ item }),
            },
            ...(hasMeaningfulConflicts(item)
              ? [
                  {
                    id: 'local',
                    label: '使用本地记录解决冲突',
                    onAction: () =>
                      setResolve({ item, policy: 'local' as const }),
                  },
                  {
                    id: 'remote',
                    label: '使用远端记录解决冲突',
                    onAction: () =>
                      setResolve({ item, policy: 'remote' as const }),
                  },
                ]
              : []),
            {
              id: 'delete',
              label: '移除域名',
              danger: true,
              onAction: () => setTarget(item),
            },
          ]}
        />
      ),
    },
  ]
  return (
    <Page
      title='DNS 托管'
      description='管理域名记录、解析线路和服务商同步。'
      actions={
        <Button size='sm' onPress={() => setCreate(true)}>
          添加域名
        </Button>
      }
    >
      <QueryNotice query={providers} onRetry={() => void providers.refetch()} />
      <ResourceList
        label='托管域名'
        query={query}
        columns={columns}
        page={page}
        onPageChange={setPage}
        pageSize={size}
        onPageSizeChange={(value) => {
          setSize(value)
          setPage(1)
        }}
        onRefresh={() => void query.refetch()}
        filters={
          <form
            className='flex flex-wrap items-end gap-2'
            onSubmit={(event) => {
              event.preventDefault()
              setKeyword(draft.trim())
              setPage(1)
            }}
          >
            <ResourceSearch
              label='搜索域名'
              value={draft}
              onChange={setDraft}
            />
            <Choice
              compact
              label='DNS 账号'
              value={provider}
              onChange={(value) => {
                setProvider(value)
                setPage(1)
              }}
              items={[
                { id: 'all', label: '全部账号' },
                ...(providers.data ?? []).map((item) => ({
                  id: item.id,
                  label: item.name,
                })),
              ]}
            />
            <Button size='sm' variant='secondary' type='submit'>
              搜索
            </Button>
            <Button
              size='sm'
              variant='ghost'
              onPress={() => {
                setDraft('')
                setKeyword('')
                setProvider('all')
                setPage(1)
              }}
            >
              重置
            </Button>
          </form>
        }
        emptyTitle='暂无托管域名'
        emptyDescription='从 DNS 服务商账号导入可用域名。'
      />
      {create && (
        <CreateZoneDialog
          providers={providers.data ?? []}
          onClose={() => setCreate(false)}
        />
      )}
      {edit && (
        <RecordsDialog
          key={edit.id}
          zone={edit}
          onClose={() => setEdit(null)}
        />
      )}
      {detail && (
        <ZoneDetailDialog
          zone={
            query.data?.list.find((item) => item.id === detail.id) ?? detail
          }
          onClose={() => setDetail(null)}
        />
      )}
      {target && (
        <Confirm
          title='移除托管域名'
          description={`确定移除“${target.domain}”？已关联网站会阻止此操作。`}
          busy={remove.isPending}
          onClose={() => setTarget(null)}
          onConfirm={() => remove.mutateAsync(target)}
        />
      )}
      {resolve && (
        <Confirm
          title='解决 DNS 冲突'
          description={`“${resolve.item.domain}”将使用${resolve.policy === 'local' ? '本地记录覆盖远端' : '远端记录替换本地'}，请确认解析目标。`}
          busy={sync.isPending}
          onClose={() => setResolve(null)}
          onConfirm={() => sync.mutateAsync(resolve)}
        />
      )}
    </Page>
  )
}
