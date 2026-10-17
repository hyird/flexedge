import { useState } from 'react'
import {
  keepPreviousData,
  useMutation,
  useQuery,
  useQueryClient,
} from '@tanstack/react-query'
import { getRouteApi, useLocation, useNavigate } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { fieldErrors } from '@/lib/form-errors'
import { formatBytes, formatDate } from '@/lib/format'
import { useListSearch } from '@/lib/list-search'
import { describeStatus, enablementStatuses, statusOptions } from '@/lib/status'
import { DataTable } from '@/components/data-table'
import { FormSheet } from '@/components/form-sheet'
import { LinkButton } from '@/components/link-button'
import { ListToolbar, FilterSelect } from '@/components/list-toolbar'
import { Page } from '@/components/page'
import { PageTabs } from '@/components/page-tabs'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { cacheCapacityLabel } from './cache_display'
import { CacheOperations } from './cache_operations'
import { CachePolicyFields } from './cache_policy_fields'
import {
  afterCachePolicyWrite,
  cachePoliciesQuery,
  saveCachePolicy,
} from './data'
import {
  cachePolicyInputSchema,
  cachePolicySchema,
  defaultCacheConfig,
  type CachePolicy,
  type CachePolicyInput,
} from './types'

const route = getRouteApi('/_authenticated/cache/')
const tabs = [
  { id: 'prewarm', label: '预热' },
  { id: 'refresh', label: '刷新' },
  { id: 'clean', label: '清理缓存' },
  { id: 'policies', label: '策略配置' },
] as const
type CacheTab = (typeof tabs)[number]['id']
export function CachePage() {
  const arrival = useLocation({
    select: (location) => location.state.cache?.tab,
  })
  const search = route.useSearch()
  const [tab, setTab] = useState<CacheTab>(() =>
    search.create === 1
      ? 'policies'
      : tabs.some((item) => item.id === arrival)
        ? (arrival as CacheTab)
        : 'policies'
  )
  return (
    <Page
      title='缓存'
      description='管理多套缓存策略，每套策略独立配置规则，由网站选择应用。'
    >
      <PageTabs
        label='缓存中心'
        items={tabs}
        selected={tab}
        onSelectionChange={setTab}
      >
        {tab === 'policies' ? (
          <CachePolicyList />
        ) : (
          <CacheOperations key={tab} operation={tab} />
        )}
      </PageTabs>
    </Page>
  )
}
function CachePolicyList() {
  const list = useListSearch(route)
  const query = useQuery({
    ...cachePoliciesQuery({
      page: list.page,
      page_size: list.pageSize,
      keyword: list.keyword || undefined,
      status: list.search.status,
    }),
    placeholderData: keepPreviousData,
  })
  const columns: DataGridColumn<CachePolicy>[] = [
    {
      id: 'name',
      header: '策略名称',
      isRowHeader: true,
      minWidth: 165,
      cell: (item) => (
        <LinkButton
          href={'/cache/' + item.id}
          variant='tertiary'
          className='justify-start px-0 font-medium'
        >
          {item.name}
        </LinkButton>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 85,
      cell: (item) => (
        <StatusChip {...describeStatus(enablementStatuses, item.status)} />
      ),
    },
    {
      id: 'storage',
      header: '存储方式',
      cell: (item) =>
        ({ tiered: '两级缓存', disk: '磁盘', memory: '内存' })[
          item.config.storage
        ],
    },
    {
      id: 'capacity',
      header: '容量 / 单对象',
      minWidth: 240,
      cell: (item) => (
        <div className='flex flex-col gap-0.5 py-1 text-xs'>
          {item.config.storage !== 'disk' && (
            <span>L1 {cacheCapacityLabel(item.config, 'memory')}</span>
          )}
          {item.config.storage !== 'memory' && (
            <span>L2 {cacheCapacityLabel(item.config, 'disk')}</span>
          )}
          <span className='text-muted'>
            单对象 {formatBytes(item.config.max_object_bytes)}
          </span>
        </div>
      ),
    },
    {
      id: 'rules',
      header: '规则',
      align: 'end',
      cell: (item) => item.config.rules.length,
    },
    {
      id: 'websites',
      header: '引用网站',
      align: 'end',
      cell: (item) => item.website_count,
    },
    {
      id: 'updated',
      header: '更新时间',
      minWidth: 160,
      cell: (item) => formatDate(item.updated_at),
    },
  ]
  return (
    <div className='flex min-w-0 flex-col gap-4'>
      <ListToolbar
        search={{
          value: list.keyword,
          onChange: (q) => void list.setSearch({ q }),
          placeholder: '搜索策略名称',
        }}
        filters={
          <FilterSelect
            label='状态'
            value={list.search.status ?? ''}
            onChange={(value) =>
              void list.setSearch({
                status: (value || undefined) as
                  'enabled' | 'disabled' | undefined,
              })
            }
            options={statusOptions(enablementStatuses)}
          />
        }
        hasFilters={!!list.keyword || !!list.search.status}
        onClearFilters={() => void list.clearFilters(['status'])}
        onRefresh={() => query.refetch()}
        isRefreshing={query.isFetching && !query.isPending}
        actions={
          <Button size='sm' onPress={() => void list.sheet.openCreate()}>
            <Plus />
            创建策略
          </Button>
        }
      />
      <DataTable
        label='缓存策略'
        columns={columns}
        data={query.data?.list}
        isLoading={query.isPending}
        error={query.isError ? query.error : undefined}
        onRetry={() => query.refetch()}
        empty={{
          title:
            list.keyword || list.search.status
              ? '没有匹配的缓存策略'
              : '尚未创建缓存策略',
          description: '创建策略并配置规则后，即可在网站中应用。',
        }}
        footer={
          <TablePagination
            page={list.page}
            pageSize={list.pageSize}
            total={query.data?.total ?? 0}
            onPageChange={(page) => void list.setPage(page)}
            onPageSizeChange={(size) => void list.setPageSize(size)}
          />
        }
      />
      {list.sheet.isCreating && (
        <CreatePolicy onClose={() => void list.sheet.close()} />
      )}
    </div>
  )
}
function CreatePolicy({ onClose }: { onClose: () => void }) {
  const client = useQueryClient(),
    navigate = useNavigate()
  const [value, setValue] = useState<CachePolicyInput>({
    name: '',
    description: '',
    status: 'enabled',
    config: defaultCacheConfig(),
  })
  const [errors, setErrors] = useState<Record<string, string>>({})
  const mutation = useMutation({
    meta: { inlineError: true },
    mutationFn: (input: CachePolicyInput) => saveCachePolicy(input),
    onSuccess: async (response) => {
      const policy = cachePolicySchema.safeParse(response.data)
      if (!policy.success) throw new Error('服务返回的策略不完整')
      toastWriteSuccess('缓存策略已创建')
      void afterCachePolicyWrite(client)
      await navigate({
        to: '/cache/$policyId',
        params: { policyId: policy.data.id },
      })
    },
  })
  return (
    <FormSheet
      isOpen
      title='创建缓存策略'
      description='预置接口绕过与静态资源缓存规则，创建后可调整顺序、条件和时间，再由网站应用。'
      onClose={onClose}
      isSubmitting={mutation.isPending}
      onSubmit={(event) => {
        event.preventDefault()
        const result = cachePolicyInputSchema.safeParse(value)
        if (!result.success) {
          setErrors(fieldErrors(result.error.issues))
          return
        }
        setErrors({})
        mutation.mutate(result.data)
      }}
      submitLabel='创建策略'
      error={mutation.error ? apiErrorMessage(mutation.error) : undefined}
    >
      <CachePolicyFields value={value} onChange={setValue} errors={errors} />
    </FormSheet>
  )
}
