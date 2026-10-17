import { useState } from 'react'
import { useParams } from '@tanstack/react-router'
import { ArrowLeft, Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Link, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTable } from '@/components/data-table'
import { Choice, Field, TextAreaField } from '@/components/forms'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { Page } from '@/components/page'
import { PageTabs } from '@/components/page-tabs'
import { SaveBar } from '@/components/save-bar'
import { Section } from '@/components/section'
import { parseExpression, referencedLists } from './expression_model'
import {
  activeMembers,
  listInputSchema,
  listKinds,
  parseListValues,
  type ListKind,
  type ListMember,
  type SecurityList,
} from './lists_model'
import { expression } from './model'
import { useSecurityData } from './security_data'
import { useSecuritySearch } from './security_navigation'

const date = (value: number) =>
  new Intl.DateTimeFormat('zh-CN', {
    timeZone: 'Asia/Singapore',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  }).format(new Date(value))
export function ListsPanel() {
  const data = useSecurityData(),
    { search, patch, router } = useSecuritySearch()
  const references = (list: SecurityList) =>
    data.policies
      .flatMap((policy) => policy.rules)
      .filter(
        (rule) =>
          (rule.enrollment.enabled && rule.enrollment.list_id === list.id) ||
          (rule.stage !== 'managed' &&
            referencedLists(parseExpression(expression(rule))).includes(
              list.name
            ))
      ).length
  const columns: DataGridColumn<SecurityList>[] = [
    {
      id: 'name',
      header: '清单名称',
      isRowHeader: true,
      cell: (list) => (
        <Link
          href={`/security/lists/${list.id}`}
          className='text-sm font-medium'
        >
          {list.description || list.name}
        </Link>
      ),
    },
    {
      id: 'identifier',
      header: '标识',
      cell: (list) => <Mono>${list.name}</Mono>,
    },
    {
      id: 'kind',
      header: '类型',
      cell: (list) => listKinds.find((item) => item.id === list.kind)?.label,
    },
    {
      id: 'members',
      header: '成员',
      cell: (list) => activeMembers(list, data.now).length,
    },
    { id: 'references', header: '规则引用', cell: references },
  ]
  return (
    <>
      <ListToolbar
        search={{
          value: search.q,
          onChange: (q) => patch({ q }),
          placeholder: '搜索清单名称或描述',
        }}
        filters={
          <FilterSelect
            label='类型'
            value={search.kind}
            onChange={(kind) => patch({ kind })}
            options={[...listKinds]}
          />
        }
        hasFilters={!!(search.q || search.kind)}
        onClearFilters={() => patch({ q: '', kind: '' })}
        actions={
          <Button
            size='sm'
            onPress={() => {
              void router.navigate({ href: '/security/lists/new' })
            }}
          >
            <Plus />
            创建清单
          </Button>
        }
      />
      <DataTable
        label='清单'
        columns={columns}
        data={data.lists.filter(
          (list) =>
            (!search.kind || list.kind === search.kind) &&
            `${list.name} ${list.description}`
              .toLowerCase()
              .includes(search.q.toLowerCase())
        )}
        isLoading={false}
        empty={{ title: '暂无匹配的清单' }}
      />
    </>
  )
}
export function SecurityListDetail() {
  const data = useSecurityData(),
    { search, patch, router } = useSecuritySearch(),
    { listId } = useParams({ strict: false }) as { listId: string }
  const creating = listId === 'new',
    [starter] = useState<SecurityList>(() => ({
      id: crypto.randomUUID(),
      name: '',
      description: '',
      kind: 'ip',
      members: [],
    })),
    [tab, setTab] = useState<'members' | 'config'>(
      creating ? 'config' : 'members'
    ),
    [adding, setAdding] = useState(false),
    [removing, setRemoving] = useState<ListMember>()
  const list = creating
    ? starter
    : data.lists.find((item) => item.id === listId)
  const back = () => {
    data.setLandingTab('lists')
    void router.navigate({ href: '/security' })
  }
  if (!list)
    return (
      <Page
        title='清单不存在'
        actions={
          <Button size='sm' onPress={back}>
            返回安全中心
          </Button>
        }
      >
        {null}
      </Page>
    )
  const columns: DataGridColumn<ListMember>[] = [
    {
      id: 'value',
      header: '成员',
      isRowHeader: true,
      cell: (member) => <Mono>{member.value}</Mono>,
    },
    { id: 'source', header: '来源', cell: (member) => member.source },
    {
      id: 'added',
      header: '加入时间（UTC+8）',
      cell: (member) => date(member.addedAt),
    },
    {
      id: 'expires',
      header: '到期时间（UTC+8）',
      cell: (member) =>
        member.expiresAt === null ? '永久' : date(member.expiresAt),
    },
    { id: 'note', header: '备注', cell: (member) => member.note || '—' },
    {
      id: 'remove',
      header: '操作',
      cell: (member) => (
        <Button
          size='sm'
          variant='tertiary'
          aria-label={`移除成员 ${member.value}`}
          onPress={() => setRemoving(member)}
        >
          移除
        </Button>
      ),
    },
  ]
  return (
    <Page
      title={creating ? '创建清单' : list.description || list.name}
      description={
        creating
          ? undefined
          : `$${list.name} · ${listKinds.find((item) => item.id === list.kind)?.label}`
      }
      actions={
        <Button size='sm' variant='tertiary' onPress={back}>
          <ArrowLeft />
          返回
        </Button>
      }
    >
      <PageTabs
        label='清单详情'
        selected={tab}
        onSelectionChange={setTab}
        items={
          creating
            ? [{ id: 'config', label: '基本配置' }]
            : [
                {
                  id: 'members',
                  label: `成员（${activeMembers(list, data.now).length}）`,
                },
                { id: 'config', label: '基本配置' },
              ]
        }
      >
        {tab === 'config' ? (
          <ListConfiguration key={list.id} list={list} creating={creating} />
        ) : (
          <>
            <ListToolbar
              search={{
                value: search.q,
                onChange: (q) => patch({ q }),
                placeholder: '搜索成员或来源',
              }}
              actions={
                <Button
                  size='sm'
                  onPress={() => setAdding((current) => !current)}
                >
                  <Plus />
                  添加成员
                </Button>
              }
            />
            {adding && (
              <MemberForm list={list} onClose={() => setAdding(false)} />
            )}
            <DataTable
              label='清单成员'
              columns={columns}
              data={activeMembers(list, data.now).filter((member) =>
                `${member.value} ${member.source}`
                  .toLowerCase()
                  .includes(search.q.toLowerCase())
              )}
              isLoading={false}
              empty={{ title: '清单暂无成员' }}
            />
          </>
        )}
      </PageTabs>
      <ConfirmDialog
        isOpen={!!removing}
        onClose={() => setRemoving(undefined)}
        title={`移除 ${removing?.value ?? ''}？`}
        description='此成员将不再参与清单匹配。'
        confirmLabel='移除成员'
        onConfirm={() =>
          data.setLists((lists) =>
            lists.map((item) =>
              item.id === list.id
                ? {
                    ...item,
                    members: item.members.filter(
                      (member) => member.id !== removing?.id
                    ),
                  }
                : item
            )
          )
        }
      />
    </Page>
  )
}
function ListConfiguration({
  list,
  creating,
}: {
  list: SecurityList
  creating: boolean
}) {
  const data = useSecurityData(),
    { router } = useSecuritySearch(),
    [value, setValue] = useState({
      name: list.name,
      description: list.description,
      kind: list.kind,
    }),
    [initialValues, setInitialValues] = useState(''),
    [error, setError] = useState(''),
    [saving, setSaving] = useState(false),
    [revision, setRevision] = useState(list.revision)
  const clean = {
    name: list.name,
    description: list.description,
    kind: list.kind,
  }
  const save = async () => {
    if (saving) return
    const result = listInputSchema.safeParse(value)
    if (!result.success) {
      setError(result.error.issues[0]?.message ?? '清单信息无效')
      return
    }
    if (
      data.lists.some(
        (item) => item.id !== list.id && item.name === result.data.name
      )
    ) {
      setError('清单名称已存在')
      return
    }
    setSaving(true)
    try {
      const now = Date.now(),
        members =
          creating && initialValues.trim()
            ? parseListValues(value.kind, initialValues).map((member) => ({
                id: crypto.randomUUID(),
                value: member,
                source: '手动添加',
                note: '',
                addedAt: now,
                expiresAt: null,
              }))
            : list.members
      const saved = await data.setLists(
        (lists) =>
          creating
            ? [...lists, { ...list, ...value, ...result.data, members }]
            : lists.map((item) =>
                item.id === list.id
                  ? { ...item, ...value, ...result.data }
                  : item
              ),
        revision
      )
      setRevision(saved.revision)
      setError('')
      setValue({ ...value, ...result.data })
      toast.success('清单已保存')
      if (creating)
        void router.navigate({
          href: `/security/lists/${saved.id}`,
          replace: true,
          ignoreBlocker: true,
        })
    } catch (reason) {
      setError(apiErrorMessage(reason))
    } finally {
      setSaving(false)
    }
  }
  return (
    <>
      <Section title='基本配置'>
        <div className='grid max-w-3xl gap-4'>
          <Field
            label='清单名称'
            value={value.name}
            disabled={!creating}
            onChange={(name) => setValue({ ...value, name })}
            placeholder='例如 malicious_ips'
            hint='小写字母、数字与下划线；以字母开头'
            required
          />
          <Field
            label='描述'
            value={value.description}
            maxLength={200}
            onChange={(description) => setValue({ ...value, description })}
            placeholder='例如 恶意来源'
          />
          <Choice
            label='清单类型'
            value={value.kind}
            disabled={!creating}
            items={[...listKinds]}
            onChange={(kind) => {
              setValue({ ...value, kind: kind as ListKind })
              setInitialValues('')
            }}
          />
          {creating && (
            <TextAreaField
              label='初始成员'
              value={initialValues}
              onChange={setInitialValues}
              placeholder={
                value.kind === 'ip'
                  ? '192.0.2.10\n203.0.113.0/24\n2001:db8::/32'
                  : value.kind === 'asn'
                    ? '64500\n64501'
                    : value.kind === 'country'
                      ? 'CN\nSG\nUS'
                      : 'curl/8.0\npython-requests/2.32'
              }
              hint='每行一项'
              rows={5}
            />
          )}
        </div>
      </Section>
      <SaveBar
        isDirty={creating || JSON.stringify(value) !== JSON.stringify(clean)}
        isSaving={saving}
        onSave={save}
        onDiscard={() => {
          setValue(clean)
          setInitialValues('')
          setError('')
        }}
        error={error}
        saveLabel={creating ? '创建清单' : '保存'}
      />
    </>
  )
}
function MemberForm({
  list,
  onClose,
}: {
  list: SecurityList
  onClose: () => void
}) {
  const data = useSecurityData(),
    [input, setInput] = useState(''),
    [note, setNote] = useState(''),
    [ttl, setTtl] = useState('permanent'),
    [error, setError] = useState(''),
    [saving, setSaving] = useState(false),
    [revision] = useState(list.revision)
  return (
    <Section title='添加成员'>
      <form
        className='flex max-w-4xl flex-col gap-4'
        noValidate
        onSubmit={async (event) => {
          event.preventDefault()
          if (saving) return
          setSaving(true)
          try {
            const values = parseListValues(list.kind, input),
              now = Date.now(),
              existing = new Set(
                activeMembers(list, now).map((item) => item.value)
              ),
              adding = values
                .filter((item) => !existing.has(item))
                .map((value) => ({
                  id: crypto.randomUUID(),
                  value,
                  source: '手动添加',
                  note,
                  addedAt: now,
                  expiresAt:
                    ttl === 'permanent' ? null : now + Number(ttl) * 1000,
                }))
            await data.setLists(
              (lists) =>
                lists.map((item) =>
                  item.id === list.id
                    ? {
                        ...item,
                        members: [...activeMembers(item, now), ...adding],
                      }
                    : item
                ),
              revision
            )
            toast.success(`已添加 ${adding.length} 项`)
            onClose()
          } catch (reason) {
            setError(apiErrorMessage(reason))
          } finally {
            setSaving(false)
          }
        }}
      >
        <TextAreaField
          label='成员'
          value={input}
          onChange={setInput}
          rows={5}
          hint='每行一项，支持批量添加'
          required
        />
        <div className='grid gap-4 sm:grid-cols-2'>
          <Choice
            label='保留时间'
            value={ttl}
            onChange={setTtl}
            items={[
              { id: 'permanent', label: '永久' },
              { id: '900', label: '15 分钟' },
              { id: '3600', label: '1 小时' },
              { id: '86400', label: '1 天' },
              { id: '604800', label: '7 天' },
            ]}
          />
          <Field label='备注' value={note} maxLength={200} onChange={setNote} />
        </div>
        {error && (
          <p role='alert' className='text-sm text-danger'>
            {error}
          </p>
        )}
        <div className='flex gap-2'>
          <Button size='sm' type='submit' isPending={saving}>
            添加成员
          </Button>
          <Button size='sm' variant='tertiary' onPress={onClose}>
            取消
          </Button>
        </div>
      </form>
    </Section>
  )
}
