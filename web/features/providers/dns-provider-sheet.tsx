import { type FormEvent, useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { fieldErrors } from '@/lib/form-errors'
import { queryKeys } from '@/lib/query-keys'
import { FormSheet } from '@/components/form-sheet'
import { Choice, Field, Notice } from '@/components/forms'
import { saveDnsProvider } from './data'
import {
  dnsProviderFormSchema,
  dnsProviderTokenLength,
  type DnsProviderFormValues,
} from './dns-provider-form'
import type { DnsProvider } from './types'

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
    meta: { inlineError: true },
    mutationFn: (input: DnsProviderFormValues) =>
      saveDnsProvider(input, provider),
    onSuccess: (response) => {
      toast.success(response.message || '已保存')
      void client.invalidateQueries({
        queryKey: [...queryKeys.providers, 'dns'],
        refetchType: 'none',
      })
      onClose()
    },
  })
  const submit = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault()
    const result = dnsProviderFormSchema(!!provider).safeParse(values)
    if (!result.success) {
      setErrors(fieldErrors(result.error.issues))
      return
    }
    setErrors({})
    mutation.mutate(result.data)
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
          : '凭据用于读取和同步托管域名的解析记录；保存后在行菜单中“验证凭据”，验证通过才能添加 DNS 区域。'
      }
      onSubmit={submit}
      submitLabel={provider ? '保存' : '添加账号'}
      isSubmitting={mutation.isPending}
      error={mutation.isError ? apiErrorMessage(mutation.error) : undefined}
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
        label={aliyun ? 'AccessKey ID' : '账户标识'}
        value={values.account_id}
        onChange={(value) => update('account_id', value)}
        required
        error={errors.account_id}
        disabled={!!provider || mutation.isPending}
        autoComplete='off'
      />
      <Field
        label={aliyun ? 'AccessKey Secret' : 'API Token'}
        type='password'
        value={values.api_token}
        onChange={(value) => update('api_token', value)}
        required={!provider}
        error={errors.api_token}
        disabled={mutation.isPending}
        hint={
          provider
            ? `留空保留当前凭据（${provider.token_hint || '已保存'}）。`
            : `${dnsProviderTokenLength[values.provider].message}。`
        }
        autoComplete='new-password'
      />
      {provider?.last_error && (
        <Notice>最近一次验证失败：{provider.last_error}</Notice>
      )}
    </FormSheet>
  )
}
