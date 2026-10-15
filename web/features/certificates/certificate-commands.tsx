import { useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { queryKeys } from '@/lib/query-keys'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { Mono } from '@/components/mono'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { websitesSnapshotKey } from '@/features/websites/data'
import {
  downloadCertificate,
  removeCertificate,
  renewCertificate,
} from './data'
import type { Certificate } from './types'

/** Renew, download and delete for the certificate detail page's cards.
 * Render `dialogs` once. Live streams push resulting state; option snapshots
 * (e.g. the website editor's usable certificates) are only marked stale, and
 * the detail page's referencing-websites snapshot re-reads once. */
export function useCertificateCommands({
  onRemoved,
}: { onRemoved?: () => void } = {}) {
  const client = useQueryClient()
  const [renewing, setRenewing] = useState<Certificate>()
  const [removing, setRemoving] = useState<Certificate>()
  const markStale = () =>
    Promise.all([
      client.invalidateQueries({
        queryKey: queryKeys.certificates,
        refetchType: 'none',
      }),
      client.invalidateQueries({ queryKey: websitesSnapshotKey }),
    ])
  const renew = useMutation({
    // Renew and delete run inside ConfirmDialog, which shows errors inline;
    // download failures fall back to the global toast.
    meta: { inlineError: true },
    mutationFn: renewCertificate,
    onSuccess: (response) => {
      toastWriteSuccess(response.message || '续期任务已提交', response.data)
      void markStale()
    },
  })
  const remove = useMutation({
    meta: { inlineError: true },
    mutationFn: removeCertificate,
    onSuccess: (response) => {
      toastWriteSuccess(response.message || '证书已删除', response.data)
      void markStale()
      onRemoved?.()
    },
  })
  const download = useMutation({
    mutationFn: downloadCertificate,
    onSuccess: ({ blob, filename }) => {
      const url = URL.createObjectURL(blob)
      const anchor = document.createElement('a')
      anchor.href = url
      anchor.download = filename
      document.body.append(anchor)
      anchor.click()
      anchor.remove()
      // Let the browser start reading the blob before releasing it.
      window.setTimeout(() => URL.revokeObjectURL(url), 1000)
    },
  })
  const primaryDomain = (certificate?: Certificate) =>
    certificate?.domains[0] ?? ''
  return {
    renew: (certificate: Certificate) => setRenewing(certificate),
    download: (certificate: Certificate) => download.mutate(certificate),
    /** Certificate id while its archive is downloading. */
    downloadingId: download.isPending ? download.variables?.id : undefined,
    remove: (certificate: Certificate) => setRemoving(certificate),
    dialogs: (
      <>
        <ConfirmDialog
          isOpen={!!renewing}
          onClose={() => setRenewing(undefined)}
          tone='accent'
          title={`立即续期证书 ${primaryDomain(renewing)}？`}
          description='将通过 DNS-01 验证重新签发证书，签发成功后自动分发到引用它的网站。当前证书在新证书生效前继续使用。'
          confirmLabel='立即续期'
          onConfirm={() => renewing && renew.mutateAsync(renewing)}
        />
        <ConfirmDialog
          isOpen={!!removing}
          onClose={() => setRemoving(undefined)}
          title={`删除证书 ${primaryDomain(removing)}？`}
          description={
            <>
              删除后无法恢复，
              <Mono>{removing?.domains.join('、')}</Mono>{' '}
              的证书文件与续期任务一并移除。仍被网站引用的证书无法删除，请先在网站
              HTTPS 配置中解除绑定。
            </>
          }
          confirmLabel='删除证书'
          onConfirm={() => removing && remove.mutateAsync(removing)}
        />
      </>
    ),
  }
}
