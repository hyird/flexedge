import { useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { Plus, TrashBin } from '@gravity-ui/icons'
import { Alert, Button, Tooltip, toast } from '@heroui/react'
import { apiErrorMessage, isRevisionConflict } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { Choice, Field, Toggle } from '@/components/forms'
import { SaveBar } from '@/components/save-bar'
import { Section } from '@/components/section'
import { saveDnsRecords } from './data'
import { dnsLinePath } from './dns-lines'
import { recordsSchema } from './records-form'
import type { DnsRecord, DnsZone } from './types'

const recordTypes: DnsRecord['type'][] = ['A', 'AAAA', 'CNAME', 'TXT', 'MX']
const maxRecords = 10000

/** In-page editor for a zone's custom records. The draft is taken when edit
 * mode opens; saving sends it with that revision, so a concurrent change
 * surfaces as a conflict instead of being overwritten silently. */
export function RecordsEditor({
  zone,
  onExit,
}: {
  zone: DnsZone
  onExit: () => void
}) {
  const client = useQueryClient()
  const [base, setBase] = useState(() => ({
    revision: zone.revision,
    records: zone.config.records,
  }))
  const [draft, setDraft] = useState<DnsRecord[]>(() =>
    structuredClone(zone.config.records)
  )
  const [errors, setErrors] = useState<Record<string, string>>({})
  const save = useMutation({
    meta: { inlineError: true },
    mutationFn: (records: DnsRecord[]) =>
      saveDnsRecords({ ...zone, revision: base.revision }, records),
    onSuccess: (response) => {
      toast.success(response.message || '记录已保存，正在同步到服务商')
      void client.invalidateQueries({
        queryKey: queryKeys.dnsZones,
        refetchType: 'none',
      })
      onExit()
    },
  })
  const isDirty = JSON.stringify(draft) !== JSON.stringify(base.records)
  const isStale = zone.revision !== base.revision
  const reload = () => {
    setBase({ revision: zone.revision, records: zone.config.records })
    setDraft(structuredClone(zone.config.records))
    setErrors({})
    save.reset()
  }
  const update = (id: string, change: Partial<DnsRecord>) => {
    setDraft((current) =>
      current.map((record) =>
        record.id === id ? { ...record, ...change } : record
      )
    )
    // An edited field's error is stale until the next save attempt.
    setErrors((current) => {
      const next = { ...current }
      for (const field of Object.keys(change)) delete next[`${id}.${field}`]
      return next
    })
  }
  const submit = () => {
    const result = recordsSchema.safeParse({ records: draft })
    if (!result.success) {
      // Keyed by record id so errors follow rows when others are removed;
      // the first issue per field wins, like `fieldErrors`.
      const next: Record<string, string> = {}
      for (const issue of result.error.issues) {
        const [, index, field] = issue.path
        const id = typeof index === 'number' ? draft[index]?.id : undefined
        next[id ? `${id}.${String(field)}` : 'records'] ??= issue.message
      }
      setErrors(next)
      return
    }
    setErrors({})
    save.mutate(result.data.records)
  }
  const errorCount = Object.keys(errors).length
  const lineItems = (current: string) => [
    ...(zone.runtime.lines.some((line) => line.code === current)
      ? []
      : [{ id: current, label: current }]),
    ...zone.runtime.lines.map((line) => ({
      id: line.code,
      label: dnsLinePath(zone.runtime.lines, line.code),
    })),
  ]
  return (
    <>
      {isStale && (
        <Alert status='warning'>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Title>区域已在其他地方更新</Alert.Title>
            <Alert.Description>
              当前草稿基于旧版本，保存会被拒绝。重新加载会放弃本页的修改。
            </Alert.Description>
          </Alert.Content>
          <Button
            size='sm'
            variant='tertiary'
            className='shrink-0 self-center'
            onPress={reload}
          >
            重新加载
          </Button>
        </Alert>
      )}
      <Section
        title={`编辑自定义记录 · ${draft.length}`}
        description='保存后立即同步到服务商。系统自动记录由集群、证书等资源生成，不能在此修改。'
        actions={
          <>
            {!isDirty && (
              <Button size='sm' variant='tertiary' onPress={onExit}>
                退出编辑
              </Button>
            )}
            <Button
              size='sm'
              variant='secondary'
              isDisabled={save.isPending || draft.length >= maxRecords}
              onPress={() =>
                setDraft((current) => [
                  ...current,
                  {
                    id: crypto.randomUUID(),
                    type: 'A',
                    name: '@',
                    content: '',
                    ttl: zone.dns_provider === 'cloudflare' ? 1 : 600,
                    proxied: false,
                    line_code: 'default',
                  },
                ])
              }
            >
              <Plus className='size-4' />
              添加记录
            </Button>
          </>
        }
      >
        {draft.length === 0 ? (
          <p className='py-6 text-center text-sm text-muted'>
            没有自定义记录。点击「添加记录」新建一条。
          </p>
        ) : (
          <ol className='flex flex-col divide-y divide-separator'>
            {draft.map((record, index) => (
              <li
                key={record.id}
                className='grid gap-3 py-4 first:pt-0 last:pb-0 sm:grid-cols-2 xl:grid-cols-[7rem_minmax(0,1fr)_minmax(0,1.6fr)_6.5rem_minmax(0,1.1fr)_auto] xl:items-start'
              >
                <Choice
                  label={`记录 ${index + 1} 类型`}
                  value={record.type}
                  onChange={(value) => {
                    const type = value as DnsRecord['type']
                    update(record.id, {
                      type,
                      priority:
                        type === 'MX' ? (record.priority ?? 10) : undefined,
                      proxied:
                        type === 'TXT' || type === 'MX'
                          ? false
                          : record.proxied,
                    })
                  }}
                  items={recordTypes.map((id) => ({ id, label: id }))}
                  disabled={save.isPending}
                />
                <Field
                  label='主机记录'
                  value={record.name}
                  onChange={(name) => update(record.id, { name })}
                  placeholder='@'
                  required
                  disabled={save.isPending}
                  error={errors[`${record.id}.name`]}
                />
                <Field
                  label='记录值'
                  value={record.content}
                  onChange={(content) => update(record.id, { content })}
                  required
                  disabled={save.isPending}
                  error={errors[`${record.id}.content`]}
                />
                <Field
                  label='TTL（秒）'
                  type='number'
                  inputMode='numeric'
                  min={1}
                  max={86400}
                  value={Number.isNaN(record.ttl) ? '' : String(record.ttl)}
                  onChange={(value) =>
                    update(record.id, { ttl: value ? Number(value) : NaN })
                  }
                  hint={
                    zone.dns_provider === 'cloudflare' && record.ttl === 1
                      ? '1 表示自动'
                      : undefined
                  }
                  disabled={save.isPending}
                  error={errors[`${record.id}.ttl`]}
                />
                <Choice
                  label='解析线路'
                  value={record.line_code}
                  onChange={(line_code) => update(record.id, { line_code })}
                  items={lineItems(record.line_code)}
                  disabled={save.isPending}
                  error={errors[`${record.id}.line_code`]}
                />
                <div className='flex items-end justify-end xl:pt-6'>
                  <Tooltip delay={300}>
                    <Button
                      size='sm'
                      variant='ghost'
                      isIconOnly
                      aria-label={`删除记录 ${index + 1}`}
                      isDisabled={save.isPending}
                      onPress={() => {
                        setDraft((current) =>
                          current.filter((item) => item.id !== record.id)
                        )
                        setErrors((current) =>
                          Object.fromEntries(
                            Object.entries(current).filter(
                              ([key]) => !key.startsWith(`${record.id}.`)
                            )
                          )
                        )
                      }}
                    >
                      <TrashBin className='size-4 text-danger' />
                    </Button>
                    <Tooltip.Content>删除记录</Tooltip.Content>
                  </Tooltip>
                </div>
                {(record.type === 'MX' ||
                  (zone.dns_provider === 'cloudflare' &&
                    record.type !== 'TXT')) && (
                  <div className='grid gap-3 sm:col-span-2 sm:grid-cols-2 xl:col-span-full xl:grid-cols-[7rem_minmax(0,1fr)_minmax(0,1.6fr)]'>
                    {record.type === 'MX' && (
                      <Field
                        label='MX 优先级'
                        type='number'
                        inputMode='numeric'
                        min={0}
                        max={65535}
                        value={
                          record.priority === undefined ||
                          Number.isNaN(record.priority)
                            ? ''
                            : String(record.priority)
                        }
                        onChange={(value) =>
                          update(record.id, {
                            priority: value ? Number(value) : NaN,
                          })
                        }
                        disabled={save.isPending}
                        error={errors[`${record.id}.priority`]}
                      />
                    )}
                    {zone.dns_provider === 'cloudflare' &&
                      record.type !== 'MX' && (
                        <div className='xl:col-span-2'>
                          <Toggle
                            label='Cloudflare 代理'
                            hint='开启后流量经 Cloudflare 转发，解析结果为 Cloudflare 地址。'
                            selected={record.proxied}
                            onChange={(proxied) =>
                              update(record.id, { proxied })
                            }
                            disabled={save.isPending}
                          />
                        </div>
                      )}
                  </div>
                )}
              </li>
            ))}
          </ol>
        )}
        {errors.records && (
          <p className='mt-3 text-sm text-danger'>{errors.records}</p>
        )}
      </Section>
      <SaveBar
        isDirty={isDirty}
        isSaving={save.isPending}
        onSave={submit}
        onDiscard={() => {
          setDraft(structuredClone(base.records))
          setErrors({})
          save.reset()
          onExit()
        }}
        saveLabel='保存并同步'
        error={
          errorCount > 0 ? (
            `${errorCount} 处输入需要修改`
          ) : save.isError && isRevisionConflict(save.error) ? (
            <>
              数据已被其他人更新。
              <Button
                size='sm'
                variant='ghost'
                className='ml-1 h-7 px-2'
                onPress={reload}
              >
                重新加载
              </Button>
            </>
          ) : save.isError ? (
            apiErrorMessage(save.error)
          ) : undefined
        }
      />
    </>
  )
}
