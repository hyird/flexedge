import { useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { Mono } from '@/components/mono'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { afterDnsZoneWrite, removeDnsZone, syncDnsZone } from './data'
import type { DnsZone } from './types'

type ConflictPolicy = 'local' | 'remote'

const policyCopy: Record<
  ConflictPolicy,
  { label: string; consequence: string }
> = {
  local: {
    label: '使用本地记录',
    consequence: '本地记录将覆盖服务商中的冲突记录。',
  },
  remote: {
    label: '使用远端记录',
    consequence: '服务商中的记录将替换本地冲突记录，本地修改会丢失。',
  },
}

/** Sync, conflict resolution and removal of a zone on the detail page.
 * Render `dialogs` once in the page. Live streams push the
 * resulting state; option snapshots are only marked stale. */
export function useZoneCommands({
  onRemoved,
}: { onRemoved?: () => void } = {}) {
  const client = useQueryClient()
  const [resolving, setResolving] = useState<{
    zone: DnsZone
    policy: ConflictPolicy
  }>()
  const [removing, setRemoving] = useState<DnsZone>()
  const onSynced = (
    response: { message: string },
    { item }: { item: Pick<DnsZone, 'id'> }
  ) => {
    toastWriteSuccess(response.message || '同步任务已提交', {
      resourceType: 'dns_zone',
      resourceId: item.id,
    })
    void afterDnsZoneWrite(client)
  }
  // "立即同步" reports failures by toast; conflict resolution and removal run
  // inside ConfirmDialog, which shows the error inline.
  const sync = useMutation({ mutationFn: syncDnsZone, onSuccess: onSynced })
  const resolveConflict = useMutation({
    meta: { inlineError: true },
    mutationFn: syncDnsZone,
    onSuccess: onSynced,
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
    /** Zone id while its sync request is in flight. */
    syncingId: sync.isPending ? sync.variables?.item.id : undefined,
    resolve: (zone: DnsZone, policy: ConflictPolicy) =>
      setResolving({ zone, policy }),
    remove: (zone: DnsZone) => setRemoving(zone),
    dialogs: (
      <>
        <ConfirmDialog
          isOpen={!!resolving}
          onClose={() => setResolving(undefined)}
          tone={resolving?.policy === 'remote' ? 'danger' : 'accent'}
          title={`${resolving ? policyCopy[resolving.policy].label : ''}解决 ${resolving?.zone.domain ?? ''} 的冲突？`}
          description={
            resolving && (
              <>
                {resolving.zone.runtime.conflicts.length} 条记录与服务商不一致。
                {policyCopy[resolving.policy].consequence}
                确认后立即提交同步任务。
              </>
            )
          }
          confirmLabel={resolving ? policyCopy[resolving.policy].label : '确认'}
          onConfirm={() =>
            resolving &&
            resolveConflict.mutateAsync({
              item: resolving.zone,
              policy: resolving.policy,
            })
          }
        />
        <ConfirmDialog
          isOpen={!!removing}
          onClose={() => setRemoving(undefined)}
          title={`移除域名 ${removing?.domain ?? ''}？`}
          description={
            <>
              移除后不再管理 <Mono>{removing?.domain}</Mono>{' '}
              的记录，服务商中的域名保留不变。仍被集群、证书或网站使用的域名无法移除。
            </>
          }
          confirmLabel='移除域名'
          onConfirm={() => removing && remove.mutateAsync(removing)}
        />
      </>
    ),
  }
}
