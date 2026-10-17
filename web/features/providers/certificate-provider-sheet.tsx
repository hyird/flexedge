import { type FormEvent, useState } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { apiErrorMessage } from '@/lib/api'
import { fieldErrors } from '@/lib/form-errors'
import { useRevisionDraft } from '@/lib/revision-draft'
import { FormSheet } from '@/components/form-sheet'
import { Choice, Field } from '@/components/forms'
import { RevisionConflictAlert } from '@/components/revision-conflict-alert'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import {
  certificateProviderFormSchema,
  type CertificateProviderFormValues,
} from './certificate-provider-form'
import { afterProviderWrite, saveCertificateProvider } from './data'
import { providerLabel } from './provider-display'
import {
  ProviderRemoval,
  ProviderVerification,
} from './provider-sheet-sections'
import type { CertificateProvider } from './types'

function toFormValues(
  provider?: CertificateProvider
): CertificateProviderFormValues {
  return {
    provider: provider?.provider ?? 'letsencrypt',
    credential_mode: provider?.credential_mode ?? 'email',
    account_email: provider?.account_email ?? '',
    access_key: '',
  }
}

/** Create (no `provider`) or edit certificate issuance credentials. The CA is
 * fixed after creation; Let's Encrypt only supports e-mail registration. */
export function CertificateProviderSheet({
  provider,
  onClose,
}: {
  provider?: CertificateProvider
  onClose: () => void
}) {
  const client = useQueryClient()
  const [edits, setEdits] = useState<CertificateProviderFormValues>()
  const [errors, setErrors] = useState<Record<string, string>>({})
  const draft = useRevisionDraft({
    server: provider,
    save: (input: CertificateProviderFormValues, revision) =>
      saveCertificateProvider(
        input,
        provider && {
          ...provider,
          revision: revision ?? provider.revision,
        }
      ),
    onSaved: (response) => {
      toastWriteSuccess(response.message || '已保存')
      void afterProviderWrite(client, 'certificate')
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
  const isDirty = (
    Object.keys(clean) as (keyof CertificateProviderFormValues)[]
  ).some((key) => values[key] !== clean[key])
  const update = (patch: Partial<CertificateProviderFormValues>) => {
    const next = { ...values, ...patch }
    const dirty = (
      Object.keys(clean) as (keyof CertificateProviderFormValues)[]
    ).some((key) => next[key] !== clean[key])
    setEdits(dirty ? next : undefined)
    draft.track(dirty)
  }
  const submit = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault()
    const result = certificateProviderFormSchema(base).safeParse(values)
    if (!result.success) {
      setErrors(fieldErrors(result.error.issues))
      return
    }
    setErrors({})
    draft.save(result.data)
  }
  return (
    <FormSheet
      isOpen
      onClose={onClose}
      title={
        provider
          ? `编辑证书服务商 ${providerLabel(provider.provider)}`
          : '添加证书服务商'
      }
      description='用于 ACME 签发与续期证书；保存后在编辑中“验证凭据”，验证通过才能申请证书。'
      onSubmit={submit}
      submitLabel={provider ? '保存' : '添加服务商'}
      isSubmitting={draft.isSaving}
      error={
        draft.error && !draft.conflict
          ? apiErrorMessage(draft.error)
          : undefined
      }
    >
      {provider && <RevisionConflictAlert draft={draft} />}
      <Choice
        label='服务商'
        value={values.provider}
        onChange={(value) =>
          update(
            value === 'letsencrypt'
              ? { provider: 'letsencrypt', credential_mode: 'email' }
              : { provider: 'zerossl' }
          )
        }
        items={[
          { id: 'letsencrypt', label: "Let's Encrypt" },
          { id: 'zerossl', label: 'ZeroSSL' },
        ]}
        disabled={!!provider || draft.isSaving}
      />
      <Choice
        label='接入方式'
        value={values.credential_mode}
        onChange={(value) =>
          update({
            credential_mode: value === 'access_key' ? 'access_key' : 'email',
          })
        }
        items={
          values.provider === 'letsencrypt'
            ? [{ id: 'email', label: '邮箱' }]
            : [
                { id: 'email', label: '邮箱' },
                { id: 'access_key', label: 'API Access Key' },
              ]
        }
        error={errors.credential_mode}
        disabled={draft.isSaving}
      />
      {values.credential_mode === 'email' ? (
        <Field
          label='账户邮箱'
          type='email'
          value={values.account_email}
          onChange={(account_email) => update({ account_email })}
          required
          error={errors.account_email}
          disabled={draft.isSaving}
          autoComplete='email'
        />
      ) : (
        <Field
          label='API Access Key'
          type='password'
          value={values.access_key}
          onChange={(access_key) => update({ access_key })}
          required={provider?.credential_mode !== 'access_key'}
          error={errors.access_key}
          disabled={draft.isSaving}
          hint={
            provider?.credential_mode === 'access_key'
              ? `留空保留当前凭据（${provider.access_key_hint || '已保存'}）。`
              : undefined
          }
          autoComplete='new-password'
        />
      )}
      {provider && (
        <>
          <ProviderVerification
            kind='certificate'
            provider={provider}
            isDirty={isDirty}
          />
          <ProviderRemoval
            kind='certificate'
            provider={provider}
            label='删除服务商'
            title={`删除证书服务商 ${providerLabel(provider.provider)}？`}
            description='删除后无法再用该凭据签发或续期证书。仍有证书使用它时，删除会被拒绝。'
            isDisabled={draft.isSaving}
            onRemoved={onClose}
          />
        </>
      )}
    </FormSheet>
  )
}
