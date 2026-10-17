import { type FormEvent, useEffect, useEffectEvent, useState } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { apiErrorMessage } from '@/lib/api'
import { useRevisionDraft } from '@/lib/revision-draft'
import { FormSheet } from '@/components/form-sheet'
import { Toggle } from '@/components/forms'
import { Mono } from '@/components/mono'
import { RevisionConflictAlert } from '@/components/revision-conflict-alert'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { afterCertificateWrite, updateCertificateRenewal } from './data'
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
  const [edits, setEdits] = useState<boolean>()
  const draft = useRevisionDraft({
    server: certificate,
    save: (value: boolean, revision) =>
      updateCertificateRenewal(
        { id: certificate.id, revision: revision ?? certificate.revision },
        value
      ),
    onSaved: (response) => {
      toastWriteSuccess(response.message || '续期设置已保存')
      void afterCertificateWrite(client)
      onClose()
    },
    load: () => setEdits(undefined),
  })
  const base = draft.base ?? certificate
  const autoRenew = edits ?? base.config.auto_renew
  const isDirty = autoRenew !== base.config.auto_renew
  const resetDraft = useEffectEvent(() => draft.discard())
  useEffect(() => {
    if (!isOpen) resetDraft()
  }, [isOpen])
  return (
    <FormSheet
      isOpen={isOpen}
      onClose={() => {
        draft.discard()
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
        draft.save(autoRenew)
      }}
      isSubmitting={draft.isSaving}
      isSubmitDisabled={!isDirty}
      error={
        draft.error && !draft.conflict
          ? apiErrorMessage(draft.error)
          : undefined
      }
    >
      <RevisionConflictAlert draft={draft} />
      <Toggle
        label='自动续期'
        hint='到期前自动重新签发，并分发到引用该证书的网站。关闭后需要手动续期。'
        selected={autoRenew}
        onChange={(next) => {
          const dirty = next !== base.config.auto_renew
          setEdits(dirty ? next : undefined)
          draft.track(dirty)
        }}
        disabled={draft.isSaving}
      />
    </FormSheet>
  )
}
