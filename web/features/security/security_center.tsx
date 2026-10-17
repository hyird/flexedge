import {
  FileText,
  LayoutHeaderCells,
  ShieldCheck,
  Sliders,
} from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button } from '@heroui/react'
import { DataTable, TruncatedText } from '@/components/data-table'
import { FormSheet } from '@/components/form-sheet'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { Page } from '@/components/page'
import { PageTabs } from '@/components/page-tabs'
import { DescriptionList } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { ListsPanel } from './lists_pages'
import { actions } from './model'
import { PoliciesPanel } from './policy_pages'
import { useSecurityData } from './security_data'
import { useSecuritySearch } from './security_navigation'
import { SecurityOverview } from './security_overview'

export function SecurityCenter() {
  const data = useSecurityData(),
    { search, patch, router } = useSecuritySearch()
  const tab = search.event ? 'events' : data.landingTab
  const filtered = data.events.filter(
    (event) =>
      (!search.action || event.action === search.action) &&
      (!search.policy || event.policyId === search.policy) &&
      `${event.ip} ${event.path} ${event.ruleName} ${event.website} ${event.listName ?? ''}`
        .toLowerCase()
        .includes(search.q.toLowerCase())
  )
  const event = data.events.find((item) => item.id === search.event)
  const time = (value: string) =>
    new Intl.DateTimeFormat('zh-CN', {
      timeZone: 'Asia/Singapore',
      month: '2-digit',
      day: '2-digit',
      hour: '2-digit',
      minute: '2-digit',
      second: '2-digit',
      hour12: false,
    }).format(new Date(value))
  const columns: DataGridColumn<(typeof data.events)[number]>[] = [
    {
      id: 'time',
      header: '时间（UTC+8）',
      cell: (item) => <Mono>{time(item.time)}</Mono>,
    },
    {
      id: 'ip',
      header: '客户端 IP',
      isRowHeader: true,
      cell: (item) => <Mono>{item.ip}</Mono>,
    },
    { id: 'website', header: '网站', cell: (item) => item.website },
    { id: 'rule', header: '命中规则', cell: (item) => item.ruleName },
    {
      id: 'action',
      header: '动作',
      cell: (item) => (
        <StatusChip
          {...actions[item.action]}
          reserve={Object.values(actions).map((action) => action.label)}
        />
      ),
    },
    {
      id: 'list',
      header: '清单联动',
      cell: (item) =>
        item.listName ? `${item.operation} $${item.listName}` : '—',
    },
    {
      id: 'detail',
      header: '操作',
      cell: (item) => (
        <Button
          size='sm'
          variant='tertiary'
          onPress={() => patch({ event: item.id }, true)}
        >
          查看详情
        </Button>
      ),
    },
  ]
  const page = Math.min(
    search.page,
    Math.max(1, Math.ceil(filtered.length / 10))
  )
  return (
    <Page title='安全' description='管理网站防护、规则集与清单。'>
      <PageTabs
        label='安全中心'
        selected={tab}
        onSelectionChange={(next) => {
          data.setLandingTab(next)
          patch({
            q: '',
            kind: '',
            policy: '',
            action: '',
            page: 1,
            event: '',
          })
        }}
        items={[
          { id: 'overview', label: '安全概览', icon: <LayoutHeaderCells /> },
          { id: 'policies', label: '规则集', icon: <ShieldCheck /> },
          { id: 'lists', label: '清单', icon: <Sliders /> },
          { id: 'events', label: '安全事件', icon: <FileText /> },
        ]}
      >
        {tab === 'policies' ? (
          <PoliciesPanel />
        ) : tab === 'lists' ? (
          <ListsPanel />
        ) : tab === 'events' ? (
          <>
            <ListToolbar
              search={{
                value: search.q,
                onChange: (q) => patch({ q, page: 1 }),
                placeholder: '搜索 IP、网站、规则或清单',
              }}
              filters={
                <>
                  <FilterSelect
                    label='规则集'
                    value={search.policy}
                    onChange={(policy) => patch({ policy, page: 1 })}
                    options={data.policies.map((item) => ({
                      id: item.id,
                      label: item.name,
                    }))}
                  />
                  <FilterSelect
                    label='动作'
                    value={search.action}
                    onChange={(action) => patch({ action, page: 1 })}
                    options={Object.entries(actions).map(([id, action]) => ({
                      id,
                      label: action.label,
                    }))}
                  />
                </>
              }
              hasFilters={!!(search.q || search.policy || search.action)}
              onClearFilters={() =>
                patch({ q: '', policy: '', action: '', page: 1 })
              }
            />
            <DataTable
              label='安全事件'
              columns={columns}
              data={filtered.slice((page - 1) * 10, page * 10)}
              isLoading={false}
              empty={{ title: '没有匹配的安全事件' }}
              footer={
                <TablePagination
                  page={page}
                  pageSize={10}
                  total={filtered.length}
                  onPageChange={(next) => patch({ page: next })}
                />
              }
            />
          </>
        ) : (
          <SecurityOverview eventColumns={columns} />
        )}
      </PageTabs>
      {event && (
        <FormSheet
          isOpen
          title='安全事件详情'
          onClose={() => patch({ event: '' })}
          description={event.id}
        >
          <DescriptionList
            items={[
              { label: '时间（UTC+8）', value: time(event.time) },
              { label: '网站', value: event.website },
              { label: '客户端 IP', value: <Mono>{event.ip}</Mono> },
              {
                label: '动作',
                value: <StatusChip {...actions[event.action]} />,
              },
              {
                label: '规则集',
                value:
                  data.policies.find((item) => item.id === event.policyId)
                    ?.name ?? '已删除',
              },
              { label: '规则', value: event.ruleName },
              {
                label: '请求',
                value: <TruncatedText text={`${event.method} ${event.path}`} />,
                wide: true,
              },
              { label: '匹配详情', value: event.detail, wide: true },
              ...(event.listName
                ? [
                    {
                      label: '清单联动',
                      value: `${event.operation} $${event.listName}`,
                      wide: true,
                    },
                  ]
                : []),
            ]}
          />
          <Button
            size='sm'
            variant='secondary'
            isDisabled={
              !data.policies.some((item) => item.id === event.policyId)
            }
            onPress={() => {
              void router.navigate({
                href: `/security/policies/${event.policyId}/rules/${encodeURIComponent(event.ruleId)}`,
              })
            }}
          >
            查看命中规则
          </Button>
        </FormSheet>
      )}
    </Page>
  )
}
