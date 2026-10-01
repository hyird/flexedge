import { useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { Drawer } from '@/components/drawer'
import { Choice, Field, FormActions, Notice } from '@/components/forms'
import { saveDnsProvider } from './data'
import {
  dnsProviderFormSchema,
  type DnsProviderFormValues,
} from './dns-provider-form'
import type { DnsProvider } from './types'

export function DnsProviderDialog({
  provider,
  onClose,
}: {
  provider?: DnsProvider
  onClose: () => void
}) {
  const client = useQueryClient()
  const [values, setValues] = useState<DnsProviderFormValues>({
    name: provider?.name ?? '',
    provider: provider?.provider === 'aliyun' ? 'aliyun' : 'cloudflare',
    account_id: provider?.account_id ?? '',
    api_token: '',
  })
  const [errors, setErrors] = useState<Record<string, string>>({})
  const update = (name: keyof DnsProviderFormValues, value: string) =>
    setValues((current) => ({ ...current, [name]: value }))
  const mutation = useMutation({
    mutationFn: (input: DnsProviderFormValues) =>
      saveDnsProvider(input, provider),
    onSuccess: (response) => {
      toast.success(response.message || '已保存')
      void client.invalidateQueries({
        queryKey: [...queryKeys.providers, 'dns'],
      })
      onClose()
    },
  })
  return (
    <Drawer
      title={provider ? '编辑 DNS 账号' : '添加 DNS 账号'}
      onClose={onClose}
      busy={mutation.isPending}
    >
      <form
        className='grid gap-4'
        onSubmit={(event) => {
          event.preventDefault()
          const result = dnsProviderFormSchema(!!provider).safeParse(values)
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
        <Field
          label='账号名称'
          value={values.name}
          onChange={(value) => update('name', value)}
          required
          error={errors.name}
          disabled={mutation.isPending}
        />
        <Choice
          label='平台'
          value={values.provider}
          onChange={(value) => update('provider', value)}
          items={[
            { id: 'cloudflare', label: 'Cloudflare' },
            { id: 'aliyun', label: '阿里云' },
          ]}
          disabled={!!provider || mutation.isPending}
        />
        <Field
          label={values.provider === 'aliyun' ? 'AccessKey ID' : '账户标识'}
          value={values.account_id}
          onChange={(value) => update('account_id', value)}
          required
          error={errors.account_id}
          disabled={!!provider || mutation.isPending}
        />
        <Field
          label={
            values.provider === 'aliyun' ? 'AccessKey Secret' : 'API Token'
          }
          type='password'
          value={values.api_token}
          onChange={(value) => update('api_token', value)}
          error={errors.api_token}
          disabled={mutation.isPending}
          hint={provider ? '留空保留当前凭据。' : '密钥至少 16 个字符。'}
          autoComplete='new-password'
        />
        {provider?.last_error && <Notice>{provider.last_error}</Notice>}
        {mutation.isError && <Notice>{apiErrorMessage(mutation.error)}</Notice>}
        <FormActions onCancel={onClose} busy={mutation.isPending} />
      </form>
    </Drawer>
  )
}
