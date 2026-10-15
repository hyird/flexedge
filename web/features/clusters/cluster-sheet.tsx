import { useEffect, useEffectEvent } from 'react'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation } from '@tanstack/react-query'
import { apiErrorMessage } from '@/lib/api'
import { FormSheet } from '@/components/form-sheet'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { ClusterAccessFields, ClusterBasicFields } from './cluster-fields'
import {
  clusterFormSchema,
  clusterFormValues,
  type ClusterFormValues,
} from './cluster-form'
import { saveCluster } from './data'

/** Create a cluster in a right sheet. The access domain preview is
 * `<prefix>.<zone>`. */
export function ClusterSheet({
  isOpen,
  onClose,
}: {
  isOpen: boolean
  onClose: () => void
}) {
  const form = useForm<ClusterFormValues>({
    resolver: zodResolver(clusterFormSchema),
    defaultValues: clusterFormValues(),
  })
  // Each opening starts from an empty draft.
  const resetForm = useEffectEvent(() => form.reset(clusterFormValues()))
  useEffect(() => {
    if (isOpen) resetForm()
  }, [isOpen])
  const mutation = useMutation({
    meta: { inlineError: true },
    mutationFn: (values: ClusterFormValues) => saveCluster(values),
    onSuccess: (response) => {
      toastWriteSuccess(response.message, response.data)
      onClose()
    },
  })
  return (
    <FormSheet
      isOpen={isOpen}
      onClose={() => {
        mutation.reset()
        onClose()
      }}
      title='创建集群'
      description='接入域名由主机前缀和托管域名组合生成，网站通过 CNAME 指向它接入集群节点。'
      onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
      submitLabel='创建集群'
      isSubmitting={mutation.isPending}
      error={mutation.isError ? apiErrorMessage(mutation.error) : undefined}
    >
      <ClusterBasicFields form={form} disabled={mutation.isPending} />
      <ClusterAccessFields
        form={form}
        disabled={mutation.isPending}
        enabled={isOpen}
      />
    </FormSheet>
  )
}
