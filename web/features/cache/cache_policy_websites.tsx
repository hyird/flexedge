import { useQuery } from '@tanstack/react-query'
import type { DataGridColumn } from '@heroui-pro/react'
import { DataTable } from '@/components/data-table'
import { LinkButton } from '@/components/link-button'
import { Section } from '@/components/section'
import { cacheWebsitesQuery } from './data'

export function CachePolicyWebsites({ policyId }: { policyId: string }) {
  const query = useQuery(cacheWebsitesQuery(policyId))
  type Item = NonNullable<typeof query.data>[number]
  const columns: DataGridColumn<Item>[] = [
    {
      id: 'name',
      header: '网站名称',
      isRowHeader: true,
      minWidth: 180,
      cell: (item) => (
        <LinkButton
          href={'/websites/' + item.id}
          variant='tertiary'
          className='justify-start px-0'
        >
          {item.name}
        </LinkButton>
      ),
    },
    {
      id: 'domains',
      header: '域名',
      minWidth: 220,
      cell: (item) => (
        <span
          className='block max-w-96 truncate'
          title={item.domains.join(', ')}
        >
          {item.domains.join(', ')}
        </span>
      ),
    },
    {
      id: 'status',
      header: '网站状态',
      cell: (item) => (item.status === 'enabled' ? '启用' : '停用'),
    },
    {
      id: 'cache',
      header: '缓存',
      cell: (item) => (item.cache_enabled ? '已启用' : '已关闭'),
    },
  ]
  return (
    <Section title='引用网站' description='策略修改会更新这些网站的发布配置。'>
      <DataTable
        label='引用网站'
        columns={columns}
        data={query.data}
        isLoading={query.isPending}
        error={query.isError ? query.error : undefined}
        onRetry={() => query.refetch()}
        variant='secondary'
        empty={{ title: '尚无网站引用此策略' }}
      />
    </Section>
  )
}
