import { useState } from 'react'
import { Controller, useForm, useWatch } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useQueryClient } from '@tanstack/react-query'
import { TrashBin } from '@gravity-ui/icons'
import {
  Button,
  FieldError,
  InputGroup,
  Label,
  TextArea,
  TextField,
} from '@heroui/react'
import { apiErrorMessage, type WriteResult } from '@/lib/api'
import { useDraftForm, useRevisionDraft } from '@/lib/revision-draft'
import { useUnsavedChangesGuard } from '@/lib/unsaved-changes'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { FormSheet } from '@/components/form-sheet'
import { Choice, Field, Toggle } from '@/components/forms'
import { RevisionConflictAlert } from '@/components/revision-conflict-alert'
import { UnsavedChangesDialog } from '@/components/save-bar'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { afterDnsZoneWrite, saveDnsRecords } from './data'
import { DnsLinePicker } from './dns-line-picker'
import {
  applyRecordChange,
  type RecordChange,
  type RecordFormValues,
  type RecordInput,
  recordFormSchema,
  recordFormValues,
  recordTypes,
} from './records-form'
import type { DnsRecord, DnsZone } from './types'

const typeItems = recordTypes.map((id) => ({ id, label: id }))

const contentLabels: Record<DnsRecord['type'], string> = {
  A: 'IPv4 地址',
  AAAA: 'IPv6 地址',
  CNAME: '目标',
  TXT: '内容',
  MX: '邮件服务器',
}

/** Sheet that adds one custom record (`record` omitted) or edits one.
 * `PUT /dns-zones/:id` takes the whole record set, so it is saved from the
 * zone revision the draft is based on: a change made elsewhere meanwhile
 * surfaces as a conflict instead of being overwritten.
 * Enter saves; closing a dirty draft asks before discarding its changes. */
export function RecordForm({
  zone,
  record,
  showLines,
  onClose,
}: {
  zone: DnsZone
  record?: DnsRecord
  /** The provider resolves per line (Aliyun); Cloudflare has only `default`. */
  showLines: boolean
  onClose: () => void
}) {
  const client = useQueryClient()
  const provider = zone.dns_provider
  const cloudflare = provider === 'cloudflare'
  const [removing, setRemoving] = useState(false)
  const [discarding, setDiscarding] = useState(false)
  const draft = useRevisionDraft<DnsZone, RecordChange, WriteResult<never>>({
    server: zone,
    save: (change: RecordChange, revision) =>
      saveDnsRecords(
        { ...base, revision: revision ?? base.revision },
        applyRecordChange(base.config.records, change)
      ),
    onSaved: (response, change) => {
      toastWriteSuccess(
        response.message ||
          (change.kind === 'remove' ? '记录已删除' : '记录已保存')
      )
      void afterDnsZoneWrite(client)
      onClose()
    },
    // Starts over from the latest records: the edited record's saved values
    // (or a blank new record); a record removed elsewhere closes the sheet.
    load: (latest) => {
      const saved = record && savedRecord(latest ?? zone)
      if (record && !saved) onClose()
      else form.reset(recordFormValues(provider, saved))
    },
  })
  const base: DnsZone = draft.base ?? zone
  const savedRecord = (source: DnsZone) =>
    record && source.config.records.find((item) => item.id === record.id)
  const form = useForm<RecordFormValues, unknown, RecordInput>({
    resolver: zodResolver(recordFormSchema(provider)),
    values: recordFormValues(provider, savedRecord(base) ?? record),
  })
  useDraftForm(form, draft.track)
  const type = useWatch({ control: form.control, name: 'type' })
  const guard = useUnsavedChangesGuard(
    form.formState.isDirty && !draft.isSaving
  )
  const submit = form.handleSubmit((values) =>
    draft.save(
      record
        ? { kind: 'edit', record: { ...values, id: record.id } }
        : { kind: 'add', record: { ...values, id: crypto.randomUUID() } }
    )
  )
  const close = () => {
    if (draft.isSaving) return
    if (form.formState.isDirty) setDiscarding(true)
    else onClose()
  }
  const disabled = draft.isSaving
  return (
    <>
      <FormSheet
        isOpen
        onClose={close}
        title={record ? `编辑记录 ${record.name} ${record.type}` : '添加记录'}
        description={`为 ${zone.domain} 配置自定义解析记录，保存时直接更新 DNS 服务商。`}
        onSubmit={submit}
        isSubmitting={draft.isSaving}
        error={
          draft.error && !draft.conflict
            ? apiErrorMessage(draft.error)
            : undefined
        }
      >
        <RevisionConflictAlert draft={draft} subject='记录' />
        <div className='grid gap-4 sm:grid-cols-2'>
          <Controller
            control={form.control}
            name='type'
            render={({ field }) => (
              <Choice
                label='类型'
                value={field.value}
                onChange={field.onChange}
                items={typeItems}
                disabled={disabled}
              />
            )}
          />
          <Controller
            control={form.control}
            name='name'
            render={({ field, fieldState }) => (
              <TextField
                value={field.value}
                onChange={field.onChange}
                isInvalid={!!fieldState.error}
                isDisabled={disabled}
                variant='secondary'
                className='w-full'
              >
                <Label>名称</Label>
                <InputGroup variant='secondary' className='w-full'>
                  <InputGroup.Input
                    placeholder='@'
                    autoFocus
                    autoComplete='off'
                    spellCheck={false}
                  />
                  <InputGroup.Suffix className='max-w-48 truncate text-muted'>
                    .{zone.domain}
                  </InputGroup.Suffix>
                </InputGroup>
                <FieldError>{fieldState.error?.message}</FieldError>
              </TextField>
            )}
          />
          <Controller
            control={form.control}
            name='content'
            render={({ field, fieldState }) =>
              type === 'TXT' ? (
                <TextField
                  value={field.value}
                  onChange={field.onChange}
                  isInvalid={!!fieldState.error}
                  isDisabled={disabled}
                  isRequired
                  variant='secondary'
                  className='w-full sm:col-span-2'
                >
                  <Label>{contentLabels[type]}</Label>
                  <TextArea
                    rows={3}
                    spellCheck={false}
                    className='w-full'
                    onKeyDown={(event) => {
                      // Enter saves like every other field; Shift+Enter adds a line.
                      if (event.key !== 'Enter' || event.shiftKey) return
                      event.preventDefault()
                      event.currentTarget.form?.requestSubmit()
                    }}
                  />
                  <FieldError>{fieldState.error?.message}</FieldError>
                </TextField>
              ) : (
                <Field
                  label={contentLabels[type]}
                  value={field.value}
                  onChange={field.onChange}
                  error={fieldState.error?.message}
                  required
                  disabled={disabled}
                  autoComplete='off'
                  className='sm:col-span-2'
                />
              )
            }
          />
        </div>
        <div className='grid gap-4 sm:grid-cols-2'>
          {showLines && (
            <div className='sm:col-span-2'>
              <Controller
                control={form.control}
                name='line_code'
                render={({ field, fieldState }) => (
                  <DnsLinePicker
                    lines={zone.runtime.lines}
                    value={field.value}
                    onChange={field.onChange}
                    error={fieldState.error?.message}
                    disabled={disabled}
                  />
                )}
              />
            </div>
          )}
          <Controller
            control={form.control}
            name='ttl'
            render={({ field, fieldState }) => (
              <Field
                label={cloudflare ? 'TTL（秒，1 为自动）' : 'TTL（秒）'}
                type='number'
                inputMode='numeric'
                min={cloudflare ? 1 : 600}
                max={86400}
                value={field.value}
                onChange={field.onChange}
                error={fieldState.error?.message}
                required
                disabled={disabled}
              />
            )}
          />
          {type === 'MX' && (
            <Controller
              control={form.control}
              name='priority'
              render={({ field, fieldState }) => (
                <Field
                  label='优先级'
                  type='number'
                  inputMode='numeric'
                  min={0}
                  max={65535}
                  value={field.value}
                  onChange={field.onChange}
                  error={fieldState.error?.message}
                  required
                  disabled={disabled}
                />
              )}
            />
          )}
          {cloudflare && type !== 'TXT' && type !== 'MX' && (
            <Controller
              control={form.control}
              name='proxied'
              render={({ field }) => (
                <div className='sm:col-span-2'>
                  <Toggle
                    label='Cloudflare 代理'
                    selected={field.value}
                    onChange={field.onChange}
                    disabled={disabled}
                  />
                </div>
              )}
            />
          )}
        </div>
        {record && (
          <Button
            size='sm'
            variant='danger-soft'
            className='self-start'
            isDisabled={disabled}
            onPress={() => setRemoving(true)}
          >
            <TrashBin className='size-4' />
            删除
          </Button>
        )}
      </FormSheet>
      {record && (
        <ConfirmDialog
          isOpen={removing}
          onClose={() => setRemoving(false)}
          title={`删除记录 ${record.name} ${record.type}？`}
          description='删除后会立即从服务商移除这条记录。'
          confirmLabel='删除记录'
          onConfirm={() => draft.saveAsync({ kind: 'remove', id: record.id })}
        />
      )}
      <UnsavedChangesDialog
        guard={{
          isBlocked: guard.isBlocked || discarding,
          stay: () => {
            if (discarding) setDiscarding(false)
            else guard.stay()
          },
          leave: () => {
            if (discarding) onClose()
            else guard.leave()
          },
        }}
      />
    </>
  )
}
