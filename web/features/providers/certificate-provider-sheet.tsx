import { type FormEvent, useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { apiErrorMessage } from '@/lib/api'
import { fieldErrors } from '@/lib/form-errors'
import { queryKeys } from '@/lib/query-keys'
import { FormSheet } from '@/components/form-sheet'
import { Choice, Field } from '@/components/forms'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import {
  certificateProviderFormSchema,
  type CertificateProviderFormValues,
} from './certificate-provider-form'
import { saveCertificateProvider } from './data'
import { providerLabel } from './provider-display'
import {
  ProviderRemoval,
  ProviderVerification,
} from './provider-sheet-sections'
import type { CertificateProvider } from './types'

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
  const [initial] = useState<CertificateProviderFormValues>(() => ({
    provider: provider?.provider === 'zerossl' ? 'zerossl' : 'letsencrypt',
    credential_mode:
      provider?.credential_mode === 'access_key' ? 'access_key' : 'email',
    account_email: provider?.account_email ?? '',
    access_key: '',
  }))
  const [values, setValues] = useState(initial)
  const isDirty = (
    Object.keys(initial) as (keyof CertificateProviderFormValues)[]
  ).some((key) => values[key] !== initial[key])
  const [errors, setErrors] = useState<Record<string, string>>({})
  const update = (patch: Partial<CertificateProviderFormValues>) =>
    setValues((current) => ({ ...current, ...patch }))
  const mutation = useMutation({
    meta: { inlineError: true },
    mutationFn: (input: CertificateProviderFormValues) =>
      saveCertificateProvider(input, provider),
    onSuccess: (response) => {
      toastWriteSuccess(response.message || '已保存', response.data)
      void client.invalidateQueries({
        queryKey: [...queryKeys.providers, 'certificate'],
        refetchType: 'none',
      })
      onClose()
    },
  })
  const submit = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault()
    const result = certificateProviderFormSchema(provider).safeParse(values)
    if (!result.success) {
      setErrors(fieldErrors(result.error.issues))
      return
    }
    setErrors({})
    mutation.mutate(result.data)
  }
  return (
    <FormSheet
      isOpen
      onClose={onClose}
      title={
        provider
          ? `编辑证书供应商 ${providerLabel(provider.provider)}`
          : '添加证书供应商'
      }
      description='用于 ACME 签发与续期证书；保存后在编辑中“验证凭据”，验证通过才能申请证书。'
      onSubmit={submit}
      submitLabel={provider ? '保存' : '添加供应商'}
      isSubmitting={mutation.isPending}
      error={mutation.isError ? apiErrorMessage(mutation.error) : undefined}
    >
      <Choice
        label='供应商'
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
        disabled={!!provider || mutation.isPending}
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
        disabled={mutation.isPending}
      />
      {values.credential_mode === 'email' ? (
        <Field
          label='账户邮箱'
          type='email'
          value={values.account_email}
          onChange={(account_email) => update({ account_email })}
          required
          error={errors.account_email}
          disabled={mutation.isPending}
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
          disabled={mutation.isPending}
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
            label='删除供应商'
            title={`删除证书供应商 ${providerLabel(provider.provider)}？`}
            description='删除后无法再用该凭据签发或续期证书。仍有证书使用它时，删除会被拒绝。'
            isDisabled={mutation.isPending}
            onRemoved={onClose}
          />
        </>
      )}
    </FormSheet>
  )
}
