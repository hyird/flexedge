import { type FormEvent, useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { apiErrorMessage } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { FormSheet } from '@/components/form-sheet'
import { Toggle } from '@/components/forms'
import { Mono } from '@/components/mono'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { websitesSnapshotKey } from '@/features/websites/data'
import { updateCertificateRenewal } from './data'
import type { Certificate } from './types'

/** "续期设置": only the automatic renewal switch is editable. */
export function RenewalSettingsSheet({
  certificate,
  isOpen,
  onClose,
}: {
  certificate: Certificate
  isOpen: boolean
  onClose: () => void
}) {
  const client = useQueryClient()
  const [autoRenew, setAutoRenew] = useState(certificate.config.auto_renew)
  const [wasOpen, setWasOpen] = useState(isOpen)
  const mutation = useMutation({
    meta: { inlineError: true },
    mutationFn: (value: boolean) =>
      updateCertificateRenewal(certificate, value),
    onSuccess: (response) => {
      toastWriteSuccess(response.message || '续期设置已保存', response.data)
      void client.invalidateQueries({
        queryKey: queryKeys.certificates,
        refetchType: 'none',
      })
      void client.invalidateQueries({ queryKey: websitesSnapshotKey })
      onClose()
    },
  })
  if (isOpen !== wasOpen) {
    setWasOpen(isOpen)
    if (isOpen) setAutoRenew(certificate.config.auto_renew)
  }
  return (
    <FormSheet
      isOpen={isOpen}
      onClose={() => {
        mutation.reset()
        onClose()
      }}
      size='sm'
      title='续期设置'
      description={
        <>
          <Mono>{certificate.domains.join('、')}</Mono>
        </>
      }
      onSubmit={(event: FormEvent<HTMLFormElement>) => {
        event.preventDefault()
        mutation.mutate(autoRenew)
      }}
      isSubmitting={mutation.isPending}
      isSubmitDisabled={autoRenew === certificate.config.auto_renew}
      error={mutation.isError ? apiErrorMessage(mutation.error) : undefined}
    >
      <Toggle
        label='自动续期'
        hint='到期前自动重新签发，并分发到引用该证书的网站。关闭后需要手动续期。'
        selected={autoRenew}
        onChange={setAutoRenew}
        disabled={mutation.isPending}
      />
    </FormSheet>
  )
}
