import { useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { ArrowRotateRight, Key } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { Section } from '@/components/section'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import {
  afterNodeWrite,
  getNodeCredentials,
  resetNodeCredentials,
  type NodeCredentials,
} from './data'
import { NodeCredentialsSheet } from './node-credentials-sheet'
import type { Node } from './types'

/** `接入凭据` card of the node config tab: reveals the agent credentials or
 * issues new ones behind a confirmation. */
export function NodeCredentialActions({ node }: { node: Node }) {
  const client = useQueryClient()
  const [credentials, setCredentials] = useState<NodeCredentials | null>(null)
  const [resetting, setResetting] = useState(false)
  const reveal = useMutation({
    mutationFn: () => getNodeCredentials(node.id),
    onSuccess: setCredentials,
  })
  return (
    <>
      <Section
        title='接入凭据'
        actions={
          <>
            <Button
              size='sm'
              variant='tertiary'
              isPending={reveal.isPending}
              onPress={() => reveal.mutate()}
            >
              <Key className='size-4' />
              查看接入凭据
            </Button>
            <Button
              size='sm'
              variant='tertiary'
              onPress={() => setResetting(true)}
            >
              <ArrowRotateRight className='size-4' />
              重置接入凭据
            </Button>
          </>
        }
      />
      <ConfirmDialog
        isOpen={resetting}
        onClose={() => setResetting(false)}
        tone='accent'
        title={`重置节点 ${node.name} 的接入凭据？`}
        description='旧凭据立即失效，节点会断开连接并回到待接入状态，需要用新凭据重新运行安装命令。'
        confirmLabel='重置凭据'
        onConfirm={async () => {
          const result = await resetNodeCredentials(node)
          // The credentials response carries the generic envelope message "ok".
          toastWriteSuccess('接入凭据已重置')
          setCredentials(result.data)
          afterNodeWrite(client)
        }}
      />
      <NodeCredentialsSheet
        credentials={credentials}
        nodeName={node.name}
        onClose={() => setCredentials(null)}
      />
    </>
  )
}
