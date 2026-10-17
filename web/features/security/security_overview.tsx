import type { DataGridColumn } from '@heroui-pro/react'
import { AreaChart } from '@heroui-pro/react/area-chart'
import { KPI } from '@heroui-pro/react/kpi'
import { Button, Link } from '@heroui/react'
import { DataTable } from '@/components/data-table'
import { Mono } from '@/components/mono'
import { Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { activeMembers } from './lists_model'
import { actions } from './model'
import { useSecurityData, type SecurityData } from './security_data'

export function SecurityOverview({
  eventColumns,
}: {
  eventColumns: DataGridColumn<SecurityData['events'][number]>[]
}) {
  const data = useSecurityData(),
    count = (value: number) => value.toLocaleString('zh-CN')
  const hourly = data.hourly
  const blocked = hourly.reduce((sum, point) => sum + point.block, 0),
    challenged = hourly.reduce((sum, point) => sum + point.challenge, 0),
    requests = data.requests
  const ranked = new Map<
    string,
    { id: string; name: string; policy: string; count: number }
  >()
  const countries = new Map<string, number>()
  for (const event of data.events) {
    const current = ranked.get(event.ruleId)
    ranked.set(event.ruleId, {
      id: event.ruleId,
      name: event.ruleName,
      policy:
        data.policies.find((policy) => policy.id === event.policyId)?.name ??
        '—',
      count: (current?.count ?? 0) + 1,
    })
    const country = /^[A-Z]{2}$/.test(event.country)
      ? (new Intl.DisplayNames(['zh-CN'], { type: 'region' }).of(
          event.country
        ) ?? event.country)
      : event.country
    countries.set(country, (countries.get(country) ?? 0) + 1)
  }
  const topRules = [...ranked.values()]
    .sort((a, b) => b.count - a.count)
    .slice(0, 5)
  const distribution = Object.entries(actions).map(([id, action]) => ({
    id,
    ...action,
    count: data.distribution[id] ?? 0,
  }))
  const totalActions = distribution.reduce(
    (sum, action) => sum + action.count,
    0
  )
  return (
    <>
      <div className='grid grid-cols-2 gap-3 lg:grid-cols-3 2xl:grid-cols-6'>
        {[
          ['24小时请求', count(requests), '访问日志已记录请求'],
          ['已阻断请求', count(blocked), '已在边缘终止'],
          ['挑战请求', count(challenged), '需要通过验证'],
          [
            '防护处置率',
            `${(((blocked + challenged) / Math.max(requests, 1)) * 100).toFixed(2)}%`,
            '阻断与挑战占比',
          ],
          [
            '应用网站',
            String(data.websites.filter((website) => website.policyId).length),
            `${data.policies.length} 套规则集`,
          ],
          [
            '清单有效成员',
            count(
              data.lists.reduce(
                (sum, list) => sum + activeMembers(list, data.now).length,
                0
              )
            ),
            `${data.lists.length} 个共享清单`,
          ],
        ].map(([label, value, detail]) => (
          <KPI key={label} className='h-26 min-w-0 gap-1 px-4 py-3'>
            <KPI.Header className='mb-0'>
              <KPI.Title>{label}</KPI.Title>
            </KPI.Header>
            <KPI.Content className='min-h-0 grid-cols-1 content-start justify-items-start gap-y-1'>
              <p className='max-w-full truncate text-2xl font-semibold tabular-nums'>
                {value}
              </p>
              <span className='max-w-full truncate text-xs text-muted'>
                {detail}
              </span>
            </KPI.Content>
          </KPI>
        ))}
      </div>
      <div className='grid gap-6 xl:grid-cols-[minmax(0,2fr)_minmax(0,1fr)]'>
        <Section
          title='防护请求趋势'
          description='近24小时 · 每小时处置数 · UTC'
          actions={
            <Button
              size='sm'
              variant='tertiary'
              onPress={() => data.setLandingTab('events')}
            >
              查看事件
            </Button>
          }
        >
          <div className='mb-3 flex gap-5 text-xs text-muted'>
            <span className='flex items-center gap-2'>
              <span className='size-2 rounded-full bg-danger' />
              阻断
            </span>
            <span className='flex items-center gap-2'>
              <span className='size-2 rounded-full bg-warning' />
              挑战
            </span>
          </div>
          <AreaChart data={hourly} height={240}>
            <AreaChart.Grid vertical={false} />
            <AreaChart.XAxis dataKey='label' minTickGap={40} />
            <AreaChart.YAxis width={48} allowDecimals={false} />
            <AreaChart.Area
              dataKey='block'
              name='阻断'
              type='monotone'
              stroke='var(--danger)'
              fill='var(--danger)'
              fillOpacity={0.1}
              dot={false}
              isAnimationActive={false}
            />
            <AreaChart.Area
              dataKey='challenge'
              name='挑战'
              type='monotone'
              stroke='var(--warning)'
              fill='var(--warning)'
              fillOpacity={0.08}
              dot={false}
              isAnimationActive={false}
            />
            <AreaChart.Tooltip />
          </AreaChart>
        </Section>
        <Section title='动作命中分布'>
          <div className='flex flex-col gap-5 py-2'>
            {distribution.map((action) => (
              <div key={action.id}>
                <div className='mb-2 flex items-center justify-between gap-3'>
                  <StatusChip label={action.label} tone={action.tone} />
                  <span className='text-sm tabular-nums'>
                    {count(action.count)}
                  </span>
                </div>
                <div
                  role='meter'
                  aria-label={`${action.label}占比`}
                  aria-valuenow={Math.round(
                    (action.count / Math.max(totalActions, 1)) * 100
                  )}
                  aria-valuemin={0}
                  aria-valuemax={100}
                  className='h-1.5 overflow-hidden rounded-full bg-default'
                >
                  <div
                    className='h-full rounded-full'
                    style={{
                      width: `${(action.count / Math.max(totalActions, 1)) * 100}%`,
                      backgroundColor: `var(--${action.tone === 'default' ? 'muted' : action.tone})`,
                    }}
                  />
                </div>
              </div>
            ))}
          </div>
        </Section>
      </div>
      <div className='grid gap-6 xl:grid-cols-2'>
        <Section title='近期规则命中'>
          <DataTable
            label='近期规则命中排行'
            columns={[
              {
                id: 'name',
                header: '规则',
                isRowHeader: true,
                cell: (rule) => rule.name,
              },
              { id: 'policy', header: '规则集', cell: (rule) => rule.policy },
              { id: 'count', header: '采样命中', cell: (rule) => rule.count },
            ]}
            data={topRules}
            isLoading={false}
            variant='secondary'
            empty={{ title: '暂无命中' }}
          />
        </Section>
        <Section title='请求来源' description='按安全事件采样统计'>
          <DataTable
            label='请求来源排行'
            columns={[
              {
                id: 'name',
                header: '国家 / 地区',
                isRowHeader: true,
                cell: (country) => country.name,
              },
              {
                id: 'count',
                header: '采样请求',
                cell: (country) => country.count,
              },
              {
                id: 'share',
                header: '占比',
                cell: (country) =>
                  `${((country.count / data.events.length) * 100).toFixed(1)}%`,
              },
            ]}
            data={[...countries.entries()]
              .map(([name, value]) => ({ id: name, name, count: value }))
              .sort((a, b) => b.count - a.count)
              .slice(0, 5)}
            isLoading={false}
            variant='secondary'
            empty={{ title: '暂无来源数据' }}
          />
        </Section>
      </div>
      <Section
        title='网站防护'
        actions={
          <Button
            size='sm'
            variant='tertiary'
            onPress={() => data.setLandingTab('policies')}
          >
            管理规则集
          </Button>
        }
      >
        <DataTable
          label='网站防护'
          columns={[
            {
              id: 'name',
              header: '网站',
              isRowHeader: true,
              cell: (website) => (
                <Link href={`/websites/${website.id}`} className='text-sm'>
                  <Mono>{website.name}</Mono>
                </Link>
              ),
            },
            {
              id: 'policy',
              header: '应用规则集',
              cell: (website) =>
                website.policyId ? (
                  <Link
                    href={`/security/policies/${website.policyId}`}
                    className='text-sm'
                  >
                    {
                      data.policies.find(
                        (policy) => policy.id === website.policyId
                      )?.name
                    }
                  </Link>
                ) : (
                  '未应用'
                ),
            },
            {
              id: 'rules',
              header: '启用规则',
              cell: (website) =>
                data.policies
                  .find((policy) => policy.id === website.policyId)
                  ?.rules.filter((rule) => rule.enabled).length ?? 0,
            },
            {
              id: 'status',
              header: '状态',
              cell: (website) => (
                <StatusChip
                  label={
                    data.policies.some(
                      (policy) => policy.id === website.policyId
                    )
                      ? '防护中'
                      : '未应用'
                  }
                  tone={
                    data.policies.some(
                      (policy) => policy.id === website.policyId
                    )
                      ? 'success'
                      : 'default'
                  }
                  reserve={['防护中', '未应用']}
                />
              ),
            },
          ]}
          data={data.websites}
          isLoading={false}
          variant='secondary'
          empty={{ title: '暂无网站' }}
        />
      </Section>
      <Section
        title='最近安全事件'
        actions={
          <Button
            size='sm'
            variant='tertiary'
            onPress={() => data.setLandingTab('events')}
          >
            查看全部
          </Button>
        }
      >
        <DataTable
          label='最近安全事件'
          columns={eventColumns}
          data={data.events.slice(0, 5)}
          isLoading={false}
          variant='secondary'
          empty={{ title: '暂无安全事件' }}
        />
      </Section>
    </>
  )
}
