import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { Link, getRouteApi } from '@tanstack/react-router'
import {
  ArrowDownToLine,
  ArrowsRotateRight,
  Eye,
  Gear,
  Plus,
  TrashBin,
} from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button } from '@heroui/react'
import { formatDate } from '@/lib/format'
import { hasListFilters, useListSearch } from '@/lib/list-search'
import { cn } from '@/lib/utils'
import { ActionMenu } from '@/components/action-menu'
import { DataTable } from '@/components/data-table'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { Page } from '@/components/page'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { providerLabel } from '@/features/providers/provider-display'
import { useCertificateCommands } from './certificate-commands'
import { certificateStatuses, isExpiring } from './certificate-display'
import { certificatesQuery } from './data'
import { RequestCertificateSheet } from './request-certificate-sheet'
import type { Certificate } from './types'

const route = getRouteApi('/_authenticated/certificates/')
const filterKeys = ['status', 'expiring', 'abnormal'] as const

export function CertificatesPage() {
  const list = useListSearch(route)
  const navigate = route.useNavigate()
  const { status, expiring, abnormal } = list.search
  const commands = useCertificateCommands()
  const query = useQuery({
    ...certificatesQuery({
      page: list.page,
      page_size: list.pageSize,
      keyword: list.keyword || undefined,
      status,
      expiring,
      abnormal,
    }),
    placeholderData: keepPreviousData,
    meta: { inlineError: true },
  })
  const filtered = hasListFilters(list.search, filterKeys)
  const setCreateOpen = (open: boolean) =>
    void navigate({
      search: (previous) => ({ ...previous, create: open ? 1 : undefined }),
      replace: true,
    })
  const createAction = (
    <Button size='sm' onPress={() => setCreateOpen(true)}>
      <Plus className='size-4' />
      申请证书
    </Button>
  )
  const attention =
    expiring === 1 ? 'expiring' : abnormal === 1 ? 'abnormal' : ''
  const columns: DataGridColumn<Certificate>[] = [
    {
      id: 'domains',
      header: '证书域名',
      isRowHeader: true,
      minWidth: 240,
      cell: (item) => (
        <div className='flex min-w-0 flex-col'>
          <Link
            to='/certificates/$certificateId'
            params={{ certificateId: item.id }}
            className='link font-medium'
          >
            <Mono truncate>{item.domains[0] ?? item.id}</Mono>
          </Link>
          {item.domains.length > 1 && (
            <span className='text-xs text-muted'>
              另含 {item.domains.length - 1} 个域名
            </span>
          )}
        </div>
      ),
    },
    {
      id: 'issuer',
      header: '签发机构',
      minWidth: 130,
      cell: (item) => item.issuer || providerLabel(item.certificate_provider),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 110,
      cell: (item) => <StatusChip status={item.status} />,
    },
    {
      id: 'expires',
      header: '有效期至',
      minWidth: 170,
      cell: (item) =>
        item.expires_at ? (
          <div className='flex flex-col'>
            <span className='tabular-nums'>
              {formatDate(item.expires_at).slice(0, 10)}
            </span>
            {item.remaining_days !== undefined && (
              <span
                className={cn(
                  'text-xs tabular-nums',
                  isExpiring(item) ? 'text-warning' : 'text-muted',
                  item.remaining_days <= 7 && 'text-danger'
                )}
              >
                剩余 {item.remaining_days} 天
              </span>
            )}
          </div>
        ) : (
          <span className='text-muted'>—</span>
        ),
    },
    {
      id: 'renewal',
      header: '续期',
      minWidth: 80,
      cell: (item) => (item.config.auto_renew ? '自动' : '手动'),
    },
    {
      id: 'websites',
      header: '引用网站',
      align: 'end',
      minWidth: 90,
      cell: (item) => (
        <span className='tabular-nums'>{item.website_count}</span>
      ),
    },
    {
      id: 'actions',
      header: '',
      align: 'end',
      width: 56,
      cell: (item) => (
        <ActionMenu
          label={`证书 ${item.domains[0] ?? ''} 的操作`}
          items={[
            {
              id: 'detail',
              label: '查看详情',
              icon: <Eye />,
              href: `/certificates/${item.id}`,
            },
            {
              id: 'settings',
              label: '续期设置',
              icon: <Gear />,
              href: `/certificates/${item.id}?renewal=1`,
            },
            {
              id: 'renew',
              label: '立即续期',
              icon: <ArrowsRotateRight />,
              disabled: item.status === 'issuing' || item.status === 'renewing',
              onAction: () => commands.renew(item),
            },
            {
              id: 'download',
              label: '下载证书',
              icon: <ArrowDownToLine />,
              disabled: !item.usable || commands.downloadingId === item.id,
              onAction: () => commands.download(item),
            },
            {
              id: 'remove',
              label: '删除证书',
              icon: <TrashBin />,
              danger: true,
              separated: true,
              onAction: () => commands.remove(item),
            },
          ]}
        />
      ),
    },
  ]
  return (
    <Page
      title='证书'
      description='通过 DNS-01 验证签发的 HTTPS 证书：有效期、自动续期与向边缘节点的分发。'
      actions={createAction}
    >
      <ListToolbar
        search={{
          value: list.keyword,
          onChange: (q) => void list.setSearch({ q }),
          placeholder: '搜索证书域名',
        }}
        filters={
          <>
            <FilterSelect
              label='状态'
              value={status ?? ''}
              onChange={(value) =>
                void list.setSearch({
                  status: (value || undefined) as typeof status,
                })
              }
              options={[...certificateStatuses]}
            />
            <FilterSelect
              label='关注'
              value={attention}
              onChange={(value) =>
                void list.setSearch({
                  expiring: value === 'expiring' ? 1 : undefined,
                  abnormal: value === 'abnormal' ? 1 : undefined,
                })
              }
              options={[
                { id: 'expiring', label: '30 天内到期' },
                { id: 'abnormal', label: '证书异常' },
              ]}
            />
          </>
        }
        hasFilters={filtered}
        onClearFilters={() => void list.clearFilters(filterKeys)}
        onRefresh={() => query.refetch()}
        isRefreshing={query.isRefetching}
      />
      <DataTable
        label='证书'
        columns={columns}
        data={query.data?.list}
        isLoading={query.isPending}
        error={query.isError ? query.error : undefined}
        onRetry={() => void query.refetch()}
        minWidth={940}
        empty={
          filtered
            ? {
                title: '没有匹配的证书',
                description: '调整搜索词或筛选条件后重试。',
              }
            : {
                title: '还没有证书',
                description: '申请证书后即可在网站 HTTPS 配置中绑定。',
                action: createAction,
              }
        }
        footer={
          <TablePagination
            page={list.page}
            pageSize={list.pageSize}
            total={query.data?.total ?? 0}
            onPageChange={list.setPage}
            onPageSizeChange={list.setPageSize}
          />
        }
      />
      <RequestCertificateSheet
        isOpen={list.search.create === 1}
        onClose={() => setCreateOpen(false)}
      />
      {commands.dialogs}
    </Page>
  )
}
