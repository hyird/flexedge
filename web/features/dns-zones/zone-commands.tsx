import { useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { Mono } from '@/components/mono'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { afterDnsZoneWrite, removeDnsZone, syncDnsZone } from './data'
import type { DnsZone } from './types'

export function useZoneCommands({
  onRemoved,
}: { onRemoved?: () => void } = {}) {
  const client = useQueryClient()
  const [removing, setRemoving] = useState<DnsZone>()
  const sync = useMutation({
    mutationFn: syncDnsZone,
    onSuccess: (response, { item }) => {
      toastWriteSuccess(response.message || '同步任务已提交', {
        resourceType: 'dns_zone',
        resourceId: item.id,
      })
      void afterDnsZoneWrite(client)
    },
  })
  const remove = useMutation({
    meta: { inlineError: true },
    mutationFn: removeDnsZone,
    onSuccess: (response, zone) => {
      toastWriteSuccess(response.message || '域名已移除', {
        resourceType: 'dns_zone',
        resourceId: zone.id,
      })
      void afterDnsZoneWrite(client)
      onRemoved?.()
    },
  })
  return {
    sync: (zone: DnsZone) => sync.mutate({ item: zone }),
    syncingId: sync.isPending ? sync.variables?.item.id : undefined,
    remove: (zone: DnsZone) => setRemoving(zone),
    dialogs: (
      <ConfirmDialog
        isOpen={!!removing}
        onClose={() => setRemoving(undefined)}
        title={`移除域名 ${removing?.domain ?? ''}？`}
        description={
          <>
            移除后不再管理 <Mono>{removing?.domain}</Mono>{' '}
            的记录，服务商中的域名和普通记录保留。仍被集群、证书或网站使用的域名无法移除。
          </>
        }
        confirmLabel='移除域名'
        onConfirm={() => removing && remove.mutateAsync(removing)}
      />
    ),
  }
}
