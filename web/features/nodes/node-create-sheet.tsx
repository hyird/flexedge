import { useState } from 'react'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { apiErrorMessage } from '@/lib/api'
import { FormSheet } from '@/components/form-sheet'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { afterNodeCreate, createNode, type CreatedNode } from './data'
import { NodeConfigFields } from './node-config-fields'
import { NodeCredentialsSheet } from './node-credentials-sheet'
import {
  nodeFormSchema,
  nodeFormValues,
  nodeInputFromForm,
  type NodeFormValues,
} from './node-form'

/** Add-node sheet → credentials sheet (install command) → node detail. */
export function NodeCreateSheet({
  isOpen,
  onClose,
  clusterId,
}: {
  isOpen: boolean
  onClose: () => void
  /** Preset and lock the cluster (cluster detail page). */
  clusterId?: string
}) {
  const client = useQueryClient()
  const navigate = useNavigate()
  const [created, setCreated] = useState<{
    node: CreatedNode
    name: string
  } | null>(null)
  const form = useForm<NodeFormValues>({
    resolver: zodResolver(nodeFormSchema),
    defaultValues: nodeFormValues(undefined, clusterId),
  })
  const mutation = useMutation({
    meta: { inlineError: true },
    mutationFn: (values: NodeFormValues) =>
      createNode(nodeInputFromForm(values)),
    onSuccess: (response, values) => {
      // The credentials response carries the generic envelope message "ok".
      // The node has no task until it registers.
      toastWriteSuccess(`节点 ${values.name} 已创建`)
      afterNodeCreate(client)
      setCreated({ node: response.data, name: values.name })
      form.reset(nodeFormValues(undefined, clusterId))
      onClose()
    },
  })
  const close = () => {
    form.reset(nodeFormValues(undefined, clusterId))
    mutation.reset()
    onClose()
  }
  const finish = () => {
    const nodeId = created?.node.id
    setCreated(null)
    if (nodeId) void navigate({ to: '/nodes/$nodeId', params: { nodeId } })
  }
  return (
    <>
      <FormSheet
        isOpen={isOpen}
        onClose={close}
        title='添加节点'
        description='创建后会生成节点接入凭据，用安装命令在服务器上启动节点程序。'
        size='lg'
        onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
        submitLabel='创建节点'
        isSubmitting={mutation.isPending}
        error={mutation.isError ? apiErrorMessage(mutation.error) : undefined}
      >
        <NodeConfigFields
          form={form}
          variant='sheet'
          disabled={mutation.isPending}
          lockCluster={clusterId !== undefined}
        />
      </FormSheet>
      <NodeCredentialsSheet
        credentials={created?.node ?? null}
        nodeName={created?.name}
        onClose={finish}
      />
    </>
  )
}
