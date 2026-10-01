import { useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { Button, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { Confirm } from '@/components/confirm'
import { Drawer } from '@/components/drawer'
import {
  Choice,
  Field,
  FormActions,
  Notice,
  TextAreaField,
  Toggle,
} from '@/components/forms'
import { saveDnsRecords } from './data'
import { dnsLinePath } from './dns-lines'
import { recordsSchema } from './records-form'
import type { DnsRecord, DnsZone } from './types'

export function RecordsDialog({
  zone,
  onClose,
}: {
  zone: DnsZone
  onClose: () => void
}) {
  const client = useQueryClient()
  const [records, setRecords] = useState<DnsRecord[]>(() =>
    structuredClone(zone.config.records)
  )
  const [errors, setErrors] = useState<Record<string, string>>({})
  const [remove, setRemove] = useState<DnsRecord | null>(null)
  const mutation = useMutation({
    mutationFn: (input: DnsRecord[]) => saveDnsRecords(zone, input),
    onSuccess: (response) => {
      toast.success(response.message || '记录已保存并提交同步')
      void client.invalidateQueries({ queryKey: queryKeys.dnsZones })
      onClose()
    },
  })
  const update = (id: string, change: Partial<DnsRecord>) =>
    setRecords((current) =>
      current.map((record) =>
        record.id === id ? { ...record, ...change } : record
      )
    )
  return (
    <>
      <Drawer
        title={`${zone.domain} · 编辑记录`}
        size='lg'
        onClose={onClose}
        busy={mutation.isPending}
      >
        <form
          className='grid gap-5'
          onSubmit={(event) => {
            event.preventDefault()
            const result = recordsSchema.safeParse({ records })
            if (!result.success) {
              setErrors(
                Object.fromEntries(
                  result.error.issues.map((issue) => [
                    issue.path.join('.'),
                    issue.message,
                  ])
                )
              )
              return
            }
            setErrors({})
            mutation.mutate(result.data.records)
          }}
        >
          <p className='text-sm text-muted'>
            保存后自动同步。系统记录由关联资源生成，不能在此编辑或删除。
          </p>
          {!!zone.runtime.projected_records.length && (
            <section className='grid gap-2'>
              <h3 className='text-sm font-medium'>系统自动记录</h3>
              {zone.runtime.projected_records.map((record) => (
                <p key={record.id} className='text-xs break-all text-muted'>
                  {record.type} · {record.name} · {record.content} · TTL{' '}
                  {record.ttl}
                </p>
              ))}
            </section>
          )}
          <section className='grid gap-4'>
            <div className='flex items-center justify-between'>
              <h3 className='text-sm font-medium'>
                自定义记录 · {records.length}
              </h3>
              <Button
                size='sm'
                variant='secondary'
                isDisabled={mutation.isPending || records.length >= 10000}
                onPress={() =>
                  setRecords((current) => [
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
                添加记录
              </Button>
            </div>
            {!records.length && (
              <p className='text-sm text-muted'>暂无自定义记录。</p>
            )}
            {records.map((record, index) => (
              <fieldset
                key={record.id}
                disabled={mutation.isPending}
                className='grid min-w-0 gap-3 rounded-xl bg-surface-secondary p-3'
              >
                <div className='flex items-center justify-between'>
                  <strong className='text-sm font-medium'>
                    记录 {index + 1}
                  </strong>
                  <Button
                    variant='danger-soft'
                    size='sm'
                    onPress={() => setRemove(record)}
                  >
                    删除
                  </Button>
                </div>
                <div className='grid gap-3 sm:grid-cols-3'>
                  <Choice
                    label='记录类型'
                    value={record.type}
                    onChange={(value) =>
                      update(record.id, {
                        type: value as DnsRecord['type'],
                        ...(value === 'MX'
                          ? { priority: record.priority ?? 10 }
                          : { priority: undefined }),
                      })
                    }
                    items={['A', 'AAAA', 'CNAME', 'TXT', 'MX'].map((id) => ({
                      id,
                      label: id,
                    }))}
                  />
                  <Field
                    label='主机记录'
                    value={record.name}
                    onChange={(value) => update(record.id, { name: value })}
                    required
                    error={errors[`records.${index}.name`]}
                  />
                  <Field
                    label='TTL（秒）'
                    type='number'
                    min={1}
                    max={86400}
                    value={String(record.ttl)}
                    onChange={(value) =>
                      update(record.id, { ttl: Number(value) })
                    }
                    error={errors[`records.${index}.ttl`]}
                  />
                </div>
                <TextAreaField
                  label='记录值'
                  rows={2}
                  value={record.content}
                  onChange={(value) => update(record.id, { content: value })}
                  required
                  error={errors[`records.${index}.content`]}
                />
                <div className='grid gap-3 sm:grid-cols-2'>
                  <Choice
                    label='解析线路'
                    value={record.line_code}
                    onChange={(value) =>
                      update(record.id, { line_code: value })
                    }
                    items={[
                      ...(!zone.runtime.lines.some(
                        (line) => line.code === record.line_code
                      )
                        ? [{ id: record.line_code, label: record.line_code }]
                        : []),
                      ...zone.runtime.lines.map((line) => ({
                        id: line.code,
                        label: dnsLinePath(zone.runtime.lines, line.code),
                      })),
                    ]}
                  />
                  {record.type === 'MX' && (
                    <Field
                      label='MX 优先级'
                      type='number'
                      min={0}
                      max={65535}
                      value={String(record.priority ?? 10)}
                      onChange={(value) =>
                        update(record.id, { priority: Number(value) })
                      }
                      error={errors[`records.${index}.priority`]}
                    />
                  )}
                </div>
                {zone.dns_provider === 'cloudflare' && (
                  <Toggle
                    label='Cloudflare 代理'
                    selected={record.proxied}
                    onChange={(value) => update(record.id, { proxied: value })}
                  />
                )}
              </fieldset>
            ))}
          </section>
          {errors.records && <Notice>{errors.records}</Notice>}
          {mutation.isError && (
            <Notice>{apiErrorMessage(mutation.error)}</Notice>
          )}
          <FormActions
            onCancel={onClose}
            busy={mutation.isPending}
            label='保存并同步'
          />
        </form>
      </Drawer>
      {remove && (
        <Confirm
          title='删除 DNS 记录'
          description={`将从当前草稿移除 ${remove.type} ${remove.name}，保存后同步到服务商。`}
          onClose={() => setRemove(null)}
          onConfirm={() => {
            setRecords((current) =>
              current.filter((record) => record.id !== remove.id)
            )
            setRemove(null)
          }}
        />
      )}
    </>
  )
}
