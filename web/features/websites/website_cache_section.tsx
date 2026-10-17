import { useQuery } from '@tanstack/react-query'
import { Choice, Notice } from '@/components/forms'
import { LinkButton } from '@/components/link-button'
import { QueryNotice } from '@/components/page'
import { Section } from '@/components/section'
import { cacheCapacityLabel } from '@/features/cache/cache_display'
import { cachePolicyOptionsQuery } from '@/features/cache/data'
import type { WebsiteSectionProps } from './website-sections'

export function WebsiteCacheSection({
  values,
  errors,
  replace,
}: WebsiteSectionProps) {
  const query = useQuery(cachePolicyOptionsQuery)
  const selected = query.data?.find(
    (item) => item.id === values.cache_policy_id
  )
  const options = (query.data ?? []).filter(
    (item) => item.status === 'enabled' || item.id === values.cache_policy_id
  )
  return (
    <div className='flex flex-col gap-6'>
      <Section
        title='缓存'
        description='网站直接应用缓存策略，缓存规则在策略中统一维护。'
        actions={
          <LinkButton href='/cache' state={{ cache: { tab: 'policies' } }}>
            管理缓存策略
          </LinkButton>
        }
      >
        <div className='flex flex-col gap-4'>
          <QueryNotice
            query={query}
            title='缓存策略加载失败'
            onRetry={() => query.refetch()}
          />
          <Choice
            label='缓存策略'
            value={values.cache_policy_id}
            onChange={(id) =>
              replace((current) => ({
                ...current,
                cache_policy_id: id,
                cache_enabled: Boolean(id),
              }))
            }
            items={[
              { id: '', label: '无' },
              ...options.map((item) => ({
                id: item.id,
                label:
                  item.name + (item.status === 'disabled' ? '（已停用）' : ''),
              })),
            ]}
            error={errors.cache_policy_id}
            hint='选择“无”不启用缓存；选择策略后，保存并分发即可应用。'
          />
          {!query.isPending && !query.isError && !query.data?.length && (
            <Notice>尚无缓存策略，请先前往缓存中心创建。</Notice>
          )}
          {selected && (
            <div className='rounded-xl border border-border p-4'>
              <div className='flex flex-wrap items-center justify-between gap-2'>
                <p className='font-medium'>{selected.name}</p>
                <LinkButton href={'/cache/' + selected.id} variant='tertiary'>
                  查看策略
                </LinkButton>
              </div>
              <p className='mt-1 text-sm text-muted'>
                {
                  { tiered: '两级', disk: '磁盘', memory: '内存' }[
                    selected.config.storage
                  ]
                }
                缓存 · 容量{' '}
                {selected.config.storage === 'tiered'
                  ? 'L1 ' +
                    cacheCapacityLabel(selected.config, 'memory') +
                    ' · L2 ' +
                    cacheCapacityLabel(selected.config, 'disk')
                  : cacheCapacityLabel(
                      selected.config,
                      selected.config.storage === 'disk' ? 'disk' : 'memory'
                    )}{' '}
                · {selected.config.rules.length} 条规则
              </p>
              <p
                className={
                  'mt-2 min-h-5 text-sm ' +
                  (selected.status === 'disabled'
                    ? 'text-warning'
                    : 'text-muted')
                }
              >
                {selected.status === 'disabled'
                  ? '策略已停用，当前绕过缓存。'
                  : '保存并分发后应用此策略。'}
              </p>
            </div>
          )}
        </div>
      </Section>
    </div>
  )
}
