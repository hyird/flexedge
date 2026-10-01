import { useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { Choice, Dialog, Field, FormActions, Notice } from '@/components/forms'
import {
  certificateProviderFormSchema,
  type CertificateProviderFormValues,
} from './certificate-provider-form'
import { saveCertificateProvider } from './data'
import type { CertificateProvider } from './types'

export function CertificateProviderDialog({
  provider,
  onClose,
}: {
  provider?: CertificateProvider
  onClose: () => void
}) {
  const client = useQueryClient()
  const [values, setValues] = useState<CertificateProviderFormValues>({
    provider: provider?.provider === 'zerossl' ? 'zerossl' : 'letsencrypt',
    credential_mode:
      provider?.credential_mode === 'access_key' ? 'access_key' : 'email',
    account_email: provider?.account_email ?? '',
    access_key: '',
  })
  const [errors, setErrors] = useState<Record<string, string>>({})
  const update = (name: keyof CertificateProviderFormValues, value: string) =>
    setValues((current) => ({ ...current, [name]: value }))
  const mutation = useMutation({
    mutationFn: (input: CertificateProviderFormValues) =>
      saveCertificateProvider(input, provider),
    onSuccess: (response) => {
      toast.success(response.message || '已保存')
      void client.invalidateQueries({
        queryKey: [...queryKeys.providers, 'certificate'],
      })
      onClose()
    },
  })
  return (
    <Dialog
      title={provider ? '编辑证书供应商' : '添加证书供应商'}
      onClose={onClose}
      busy={mutation.isPending}
    >
      <form
        className='grid gap-4'
        onSubmit={(event) => {
          event.preventDefault()
          const result =
            certificateProviderFormSchema(provider).safeParse(values)
          if (!result.success) {
            setErrors(
              Object.fromEntries(
                result.error.issues.map((issue) => [
                  String(issue.path[0]),
                  issue.message,
                ])
              )
            )
            return
          }
          setErrors({})
          mutation.mutate(result.data)
        }}
      >
        <Choice
          label='供应商'
          value={values.provider}
          onChange={(value) => {
            update('provider', value)
            if (value === 'letsencrypt') update('credential_mode', 'email')
          }}
          items={[
            { id: 'letsencrypt', label: "Let's Encrypt" },
            { id: 'zerossl', label: 'ZeroSSL' },
          ]}
          disabled={!!provider || mutation.isPending}
        />
        <Choice
          label='接入方式'
          value={values.credential_mode}
          onChange={(value) => update('credential_mode', value)}
          items={
            values.provider === 'letsencrypt'
              ? [{ id: 'email', label: '邮箱' }]
              : [
                  { id: 'email', label: '邮箱' },
                  { id: 'access_key', label: 'API Access Key' },
                ]
          }
          disabled={mutation.isPending}
        />
        {values.credential_mode === 'email' ? (
          <Field
            label='账户邮箱'
            type='email'
            value={values.account_email}
            onChange={(value) => update('account_email', value)}
            required
            error={errors.account_email}
            disabled={mutation.isPending}
          />
        ) : (
          <Field
            label='API Access Key'
            type='password'
            value={values.access_key}
            onChange={(value) => update('access_key', value)}
            error={errors.access_key}
            disabled={mutation.isPending}
            hint={
              provider?.credential_mode === 'access_key'
                ? '留空保留当前凭据。'
                : undefined
            }
            autoComplete='new-password'
          />
        )}
        {errors.credential_mode && <Notice>{errors.credential_mode}</Notice>}
        {provider?.last_error && <Notice>{provider.last_error}</Notice>}
        {mutation.isError && <Notice>{apiErrorMessage(mutation.error)}</Notice>}
        <FormActions onCancel={onClose} busy={mutation.isPending} />
      </form>
    </Dialog>
  )
}
