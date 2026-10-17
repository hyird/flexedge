import { type FormEvent, useState } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { apiErrorMessage } from '@/lib/api'
import { fieldErrors } from '@/lib/form-errors'
import { useRevisionDraft } from '@/lib/revision-draft'
import { FormSheet } from '@/components/form-sheet'
import { Choice, Field } from '@/components/forms'
import { RevisionConflictAlert } from '@/components/revision-conflict-alert'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { afterProviderWrite, saveDnsProvider } from './data'
import {
  dnsProviderFormSchema,
  dnsProviderTokenLength,
  type DnsProviderFormValues,
} from './dns-provider-form'
import { ProviderRemoval } from './provider-sheet-sections'
import type { DnsProvider } from './types'

function toFormValues(provider?: DnsProvider): DnsProviderFormValues {
  return {
    name: provider?.name ?? '',
    provider: provider?.provider ?? 'cloudflare',
    account_id: provider?.account_id ?? '',
    api_token: '',
  }
}

/** Create (no `provider`) or edit a DNS account. Platform and account id are
 * fixed after creation; an empty secret keeps the stored credential. */
export function DnsProviderSheet({
  provider,
  onClose,
}: {
  provider?: DnsProvider
  onClose: () => void
}) {
  const client = useQueryClient()
  const [edits, setEdits] = useState<DnsProviderFormValues>()
  const [errors, setErrors] = useState<Record<string, string>>({})
  const draft = useRevisionDraft({
    server: provider,
    save: (input: DnsProviderFormValues, revision) =>
      saveDnsProvider(
        input,
        provider && {
          ...provider,
          revision: revision ?? provider.revision,
        }
      ),
    onSaved: (response) => {
      toastWriteSuccess(response.message || '已保存')
      void afterProviderWrite(client, 'dns')
      onClose()
    },
    load: () => {
      setEdits(undefined)
      setErrors({})
    },
  })
  const base = draft.base ?? provider
  const clean = toFormValues(base)
  const values = edits ?? clean
  const update = (name: keyof DnsProviderFormValues, value: string) => {
    const next = { ...values, [name]: value }
    const dirty = (Object.keys(clean) as (keyof DnsProviderFormValues)[]).some(
      (key) => next[key] !== clean[key]
    )
    setEdits(dirty ? next : undefined)
    draft.track(dirty)
  }
  const submit = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault()
    const result = dnsProviderFormSchema(!!provider).safeParse(values)
    if (!result.success) {
      setErrors(fieldErrors(result.error.issues))
      return
    }
    setErrors({})
    draft.save(result.data)
  }
  const aliyun = values.provider === 'aliyun'
  return (
    <FormSheet
      isOpen
      onClose={onClose}
      title={provider ? `编辑 DNS 账号 ${provider.name}` : '添加 DNS 账号'}
      description={
        provider
          ? '可修改账号名称或轮换访问密钥；平台和账户标识创建后不可修改。'
          : '凭据用于读取和同步托管域名的解析记录；保存后在列表中验证，验证通过才能添加域名。'
      }
      onSubmit={submit}
      submitLabel={provider ? '保存' : '添加账号'}
      isSubmitting={draft.isSaving}
      error={
        draft.error && !draft.conflict
          ? apiErrorMessage(draft.error)
          : undefined
      }
    >
      {provider && <RevisionConflictAlert draft={draft} />}
      <Field
        label='账号名称'
        value={values.name}
        onChange={(value) => update('name', value)}
        required
        error={errors.name}
        disabled={draft.isSaving}
      />
      <Choice
        label='平台'
        value={values.provider}
        onChange={(value) => update('provider', value)}
        items={[
          { id: 'cloudflare', label: 'Cloudflare' },
          { id: 'aliyun', label: '阿里云' },
        ]}
        disabled={!!provider || draft.isSaving}
      />
      <Field
        label={aliyun ? 'AccessKey ID' : '账户标识'}
        value={values.account_id}
        onChange={(value) => update('account_id', value)}
        required
        error={errors.account_id}
        disabled={!!provider || draft.isSaving}
        autoComplete='off'
      />
      <Field
        label={aliyun ? 'AccessKey Secret' : 'API Token'}
        type='password'
        value={values.api_token}
        onChange={(value) => update('api_token', value)}
        required={!provider}
        error={errors.api_token}
        disabled={draft.isSaving}
        hint={
          provider
            ? `留空保留当前凭据（${provider.token_hint || '已保存'}）。`
            : `${dnsProviderTokenLength[values.provider].message}。`
        }
        autoComplete='new-password'
      />
      {provider && (
        <>
          <ProviderRemoval
            kind='dns'
            provider={provider}
            label='删除账号'
            title={`删除 DNS 账号 ${provider.name}？`}
            description='删除后将无法再用该账号同步解析记录。仍有托管域名时，删除会被拒绝。'
            isDisabled={draft.isSaving}
            onRemoved={onClose}
          />
        </>
      )}
    </FormSheet>
  )
}
