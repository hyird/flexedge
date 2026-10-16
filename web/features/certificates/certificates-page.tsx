import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button } from '@heroui/react'
import { formatDate } from '@/lib/format'
import { hasListFilters, useListSearch } from '@/lib/list-search'
import { describeStatus, statusOptions } from '@/lib/status'
import { cn } from '@/lib/utils'
import { DataTable } from '@/components/data-table'
import { LinkButton } from '@/components/link-button'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { Page } from '@/components/page'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { providerLabel } from '@/features/providers/provider-display'
import {
  certificateStatuses,
  certificateStatusReserve,
  isExpiring,
} from './certificate-display'
import { certificateFilterKeys } from './certificate-search'
import { certificatesQuery } from './data'
import { RequestCertificateSheet } from './request-certificate-sheet'
import type { Certificate } from './types'

const route = getRouteApi('/_authenticated/certificates/')
const statusFilterOptions = statusOptions(certificateStatuses)

export function CertificatesPage() {
  const list = useListSearch(route)
  const { status, expiring, abnormal } = list.search
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
  const filtered = hasListFilters(list.search, certificateFilterKeys)
  const createAction = (
    <Button size='sm' onPress={() => void list.sheet.openCreate()}>
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
        <div className='flex min-w-0 items-center gap-2'>
          <Mono truncate className='min-w-0 font-medium'>
            {item.domains[0] ?? item.id}
          </Mono>
          {item.domains.length > 1 && (
            <span
              className='shrink-0 text-xs text-muted tabular-nums'
              title={item.domains.slice(1).join('\n')}
            >
              +{item.domains.length - 1}
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
      cell: (item) => (
        <StatusChip
          {...describeStatus(certificateStatuses, item.status)}
          reserve={certificateStatusReserve}
        />
      ),
    },
    {
      id: 'expires',
      header: '有效期至',
      minWidth: 190,
      // One line: a date slot as wide as `0000-00-00` (or `—`), then the
      // remaining days at a fixed position.
      cell: (item) => (
        <span className='flex items-center gap-2 tabular-nums'>
          <span className='inline-grid'>
            <span aria-hidden className='invisible col-start-1 row-start-1'>
              0000-00-00
            </span>
            <span
              className={cn(
                'col-start-1 row-start-1',
                !item.expires_at && 'text-muted'
              )}
            >
              {item.expires_at ? formatDate(item.expires_at).slice(0, 10) : '—'}
            </span>
          </span>
          {item.expires_at && item.remaining_days !== undefined && (
            <span
              className={cn(
                'text-xs',
                isExpiring(item) ? 'text-warning' : 'text-muted',
                item.remaining_days <= 7 && 'text-danger'
              )}
            >
              剩余 {item.remaining_days} 天
            </span>
          )}
        </span>
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
      header: <span className='sr-only'>操作</span>,
      width: 72,
      cell: (item) => (
        <LinkButton
          href={`/certificates/${item.id}`}
          className='px-2'
          aria-label={`证书 ${item.domains[0] ?? ''} 的详情`}
        >
          详情
        </LinkButton>
      ),
    },
  ]
  return (
    <Page
      title='证书'
      description='通过 DNS-01 验证签发的 HTTPS 证书：有效期、自动续期与向边缘节点的分发。'
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
              options={statusFilterOptions}
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
        onClearFilters={() => void list.clearFilters(certificateFilterKeys)}
        onRefresh={() => query.refetch()}
        isRefreshing={query.isRefetching}
        actions={createAction}
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
        isOpen={list.sheet.isCreating}
        onClose={() => void list.sheet.close()}
      />
    </Page>
  )
}
