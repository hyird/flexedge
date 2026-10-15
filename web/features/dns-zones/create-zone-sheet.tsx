import { type FormEvent, useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { ArrowRotateLeft } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { FormSheet } from '@/components/form-sheet'
import { Choice, Notice } from '@/components/forms'
import { QueryNotice } from '@/components/page'
import { dnsProviderOptionsQuery } from '@/features/providers/data'
import { providerLabel } from '@/features/providers/provider-display'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { availableDnsZonesQuery, createDnsZone } from './data'

/** "添加域名": pick a DNS account, then one of its not-yet-managed zones. */
export function CreateZoneSheet({
  isOpen,
  onClose,
}: {
  isOpen: boolean
  onClose: () => void
}) {
  const client = useQueryClient()
  const [provider, setProvider] = useState('')
  const [domain, setDomain] = useState('')
  const [errors, setErrors] = useState<{ provider?: string; domain?: string }>(
    {}
  )
  const [wasOpen, setWasOpen] = useState(isOpen)
  const providers = useQuery({
    ...dnsProviderOptionsQuery,
    enabled: isOpen,
    gcTime: 0,
    staleTime: 0,
    meta: { inlineError: true },
  })
  const available = useQuery({
    ...availableDnsZonesQuery(provider),
    enabled: isOpen && !!provider,
    gcTime: 0,
    staleTime: 0,
    meta: { inlineError: true },
  })
  const mutation = useMutation({
    meta: { inlineError: true },
    mutationFn: createDnsZone,
    onSuccess: (response) => {
      toastWriteSuccess(
        response.message || '域名已添加，正在同步',
        response.data
      )
      void client.invalidateQueries({
        queryKey: queryKeys.dnsZones,
        refetchType: 'none',
      })
      onClose()
    },
  })
  // Each opening starts from an empty form.
  if (isOpen !== wasOpen) {
    setWasOpen(isOpen)
    if (isOpen) {
      setProvider('')
      setDomain('')
      setErrors({})
    }
  }
  const close = () => {
    mutation.reset()
    onClose()
  }
  const submit = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault()
    const next = {
      provider: provider ? undefined : '请选择 DNS 账号',
      domain: domain ? undefined : '请选择要托管的域名',
    }
    setErrors(next)
    if (!next.provider && !next.domain)
      mutation.mutate({ dns_provider_id: provider, domain })
  }
  return (
    <FormSheet
      isOpen={isOpen}
      onClose={close}
      title='添加域名'
      description='从 DNS 账号导入尚未托管的域名，添加后立即同步记录与解析线路。'
      onSubmit={submit}
      submitLabel='添加并同步'
      isSubmitting={mutation.isPending}
      error={mutation.isError ? apiErrorMessage(mutation.error) : undefined}
    >
      <QueryNotice
        query={providers}
        onRetry={() => void providers.refetch()}
        title='DNS 账号加载失败'
      />
      <Choice
        label='DNS 账号'
        required
        value={provider}
        onChange={(value) => {
          setProvider(value)
          setDomain('')
          setErrors({})
        }}
        items={(providers.data ?? []).map((item) => ({
          id: item.id,
          label: `${item.name} · ${providerLabel(item.provider)}`,
        }))}
        disabled={providers.isPending || mutation.isPending}
        error={errors.provider}
      />
      {providers.isSuccess && !providers.data.length && (
        <Notice>
          还没有 DNS 账号，请先在「供应商账号 › DNS 账号」中添加。
        </Notice>
      )}
      <Choice
        label='域名'
        required
        value={domain}
        onChange={(value) => {
          setDomain(value)
          setErrors((current) => ({ ...current, domain: undefined }))
        }}
        items={(available.data ?? []).map((zone) => ({
          id: zone.domain,
          label: zone.domain,
        }))}
        disabled={!provider || available.isPending || mutation.isPending}
        hint={
          !provider
            ? '先选择 DNS 账号'
            : available.isPending
              ? '正在查询账号下的可用域名…'
              : undefined
        }
        error={errors.domain}
      />
      {provider && (
        <Button
          size='sm'
          variant='tertiary'
          className='self-start'
          isPending={available.isFetching}
          isDisabled={mutation.isPending}
          onPress={() => void available.refetch()}
        >
          <ArrowRotateLeft className='size-4' />
          重新查询可用域名
        </Button>
      )}
      <QueryNotice
        query={available}
        onRetry={() => void available.refetch()}
        title='可用域名查询失败'
      />
      {provider && available.isSuccess && !available.data.length && (
        <Notice>此账号下没有可导入的域名，请先在服务商中添加域名。</Notice>
      )}
    </FormSheet>
  )
}
