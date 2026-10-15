import { type KeyboardEvent, useState } from 'react'
import { Controller, useForm, useWatch } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { TrashBin } from '@gravity-ui/icons'
import {
  Alert,
  Button,
  FieldError,
  InputGroup,
  Label,
  TextArea,
  TextField,
  toast,
} from '@heroui/react'
import { apiErrorMessage, isRevisionConflict } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { useUnsavedChangesGuard } from '@/lib/unsaved-changes'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { Choice, Field, Toggle } from '@/components/forms'
import { UnsavedChangesDialog } from '@/components/save-bar'
import { saveDnsRecords } from './data'
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

/** Inline form that adds one custom record (`record` omitted) or edits one.
 * The zone revision is pinned when the form opens and the whole record set
 * is saved with it (`PUT /dns-zones/:id` takes the full set), so a change made
 * elsewhere meanwhile surfaces as a conflict instead of being overwritten.
 * Enter saves, Esc cancels. Only this form re-renders while typing. */
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
  const [baseRevision, setBaseRevision] = useState(zone.revision)
  const [removing, setRemoving] = useState(false)
  const form = useForm<RecordFormValues, unknown, RecordInput>({
    resolver: zodResolver(recordFormSchema(provider)),
    defaultValues: recordFormValues(provider, record),
  })
  const type = useWatch({ control: form.control, name: 'type' })
  const save = useMutation({
    meta: { inlineError: true },
    mutationFn: (change: RecordChange) =>
      saveDnsRecords(
        { ...zone, revision: baseRevision },
        applyRecordChange(zone.config.records, change)
      ),
    onSuccess: (response, change) => {
      toast.success(
        response.message ||
          (change.kind === 'remove'
            ? '记录已删除，正在同步到服务商'
            : '记录已保存，正在同步到服务商')
      )
      void client.invalidateQueries({
        queryKey: queryKeys.dnsZones,
        refetchType: 'none',
      })
      onClose()
    },
  })
  const guard = useUnsavedChangesGuard(
    form.formState.isDirty && !save.isPending
  )
  const isStale = zone.revision !== baseRevision
  const isConflict = save.isError && isRevisionConflict(save.error)
  // Starts over from the latest records: the edited record's saved values (or
  // a blank new record) on the current revision; unsaved input is dropped.
  const reload = () => {
    const latest = record
      ? zone.config.records.find((item) => item.id === record.id)
      : undefined
    if (record && !latest) {
      onClose()
      return
    }
    setBaseRevision(zone.revision)
    form.reset(recordFormValues(provider, latest))
    save.reset()
  }
  const submit = form.handleSubmit((values) =>
    save.mutate(
      record
        ? { kind: 'edit', record: { ...values, id: record.id } }
        : { kind: 'add', record: { ...values, id: crypto.randomUUID() } }
    )
  )
  const cancelOnEscape = (event: KeyboardEvent<HTMLFormElement>) => {
    // An open picker closes first; dialogs and popovers are portals whose
    // keys still bubble through the React tree, so only the form's own
    // fields cancel it.
    const target = event.target as HTMLElement
    if (
      event.key !== 'Escape' ||
      event.defaultPrevented ||
      save.isPending ||
      !event.currentTarget.contains(target) ||
      target.getAttribute('aria-expanded') === 'true'
    )
      return
    event.preventDefault()
    onClose()
  }
  const disabled = save.isPending
  return (
    <form
      noValidate
      aria-label={record ? `编辑记录 ${record.name}` : '添加记录'}
      className='flex flex-col gap-4 py-2'
      onSubmit={(event) => {
        event.preventDefault()
        void submit()
      }}
      onKeyDown={cancelOnEscape}
    >
      {(isStale || isConflict) && (
        <Alert status='warning'>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Title>记录已被其他人更新</Alert.Title>
            <Alert.Description>
              重新加载会载入最新记录并丢弃这里未保存的修改。
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
      <div className='grid gap-3 sm:grid-cols-2 lg:grid-cols-[8rem_minmax(0,1fr)_minmax(0,1.6fr)] lg:items-start'>
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
                className='w-full sm:col-span-2 lg:col-span-1'
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
                className='sm:col-span-2 lg:col-span-1'
              />
            )
          }
        />
      </div>
      <div className='grid gap-3 sm:grid-cols-2 lg:grid-cols-[minmax(0,1.6fr)_8rem_8rem_minmax(0,1fr)] lg:items-start'>
        {showLines && (
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
              <div className='lg:pt-5'>
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
      <div className='flex flex-wrap items-center gap-2'>
        {record && (
          <Button
            size='sm'
            variant='danger-soft'
            isDisabled={disabled}
            onPress={() => setRemoving(true)}
          >
            <TrashBin className='size-4' />
            删除
          </Button>
        )}
        <span className='ms-auto min-w-0 text-sm text-danger' role='alert'>
          {save.isError && !isConflict ? apiErrorMessage(save.error) : null}
        </span>
        <Button
          size='sm'
          variant='tertiary'
          isDisabled={disabled}
          onPress={onClose}
        >
          取消
        </Button>
        <Button
          size='sm'
          type='submit'
          isPending={save.isPending && !removing}
          isDisabled={disabled}
        >
          保存
        </Button>
      </div>
      {record && (
        <ConfirmDialog
          isOpen={removing}
          onClose={() => setRemoving(false)}
          title={`删除记录 ${record.name} ${record.type}？`}
          description='删除后会立即从服务商移除这条记录。'
          confirmLabel='删除记录'
          onConfirm={() => save.mutateAsync({ kind: 'remove', id: record.id })}
        />
      )}
      <UnsavedChangesDialog guard={guard} />
    </form>
  )
}
