import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Button, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { Choice, Dialog, Notice } from '@/components/forms'
import { QueryNotice } from '@/components/page'
import type { DnsProvider } from '@/features/providers/types'
import { availableDnsZonesQuery, createDnsZone } from './data'

export function CreateZoneDialog({
  providers,
  onClose,
}: {
  providers: DnsProvider[]
  onClose: () => void
}) {
  const client = useQueryClient()
  const [provider, setProvider] = useState('')
  const [domain, setDomain] = useState('')
  const available = useQuery(availableDnsZonesQuery(provider))
  const mutation = useMutation({
    mutationFn: createDnsZone,
    onSuccess: (response) => {
      toast.success(response.message || '已添加并提交同步')
      void client.invalidateQueries({ queryKey: queryKeys.dnsZones })
      onClose()
    },
  })
  const selectable =
    !!provider && !!available.data?.some((zone) => zone.domain === domain)
  return (
    <Dialog title='添加托管域名' onClose={onClose} busy={mutation.isPending}>
      <form
        className='grid gap-4'
        onSubmit={(event) => {
          event.preventDefault()
          if (selectable) mutation.mutate({ dns_provider_id: provider, domain })
        }}
      >
        <p className='text-sm text-muted'>
          从 DNS 服务商账号导入尚未托管的区域。
        </p>
        <Choice
          label='DNS 账号'
          required
          value={provider}
          onChange={(value) => {
            setProvider(value)
            setDomain('')
          }}
          items={providers.map((item) => ({ id: item.id, label: item.name }))}
          disabled={mutation.isPending}
        />
        {!providers.length && <Notice>请先添加 DNS 服务商账号。</Notice>}
        <Choice
          label='可用域名'
          required
          value={domain}
          onChange={setDomain}
          items={(available.data ?? []).map((zone) => ({
            id: zone.domain,
            label: zone.domain,
          }))}
          disabled={!provider || available.isPending || mutation.isPending}
        />
        {provider && available.isPending && (
          <p role='status' className='text-sm text-muted'>
            正在查询可用域名…
          </p>
        )}
        <QueryNotice
          query={available}
          onRetry={() => void available.refetch()}
        />
        {provider && available.isSuccess && !available.data.length && (
          <Notice>此账号暂无可导入域名，请确认已在服务商中创建区域。</Notice>
        )}
        {provider && (
          <Button
            size='sm'
            variant='ghost'
            isDisabled={available.isFetching}
            onPress={() => void available.refetch()}
          >
            刷新可用域名
          </Button>
        )}
        {mutation.isError && <Notice>{apiErrorMessage(mutation.error)}</Notice>}
        <div className='flex justify-end gap-2'>
          <Button
            variant='secondary'
            onPress={onClose}
            isDisabled={mutation.isPending}
          >
            取消
          </Button>
          <Button
            type='submit'
            isDisabled={!selectable || mutation.isPending}
            isPending={mutation.isPending}
          >
            添加并同步
          </Button>
        </div>
      </form>
    </Dialog>
  )
}
