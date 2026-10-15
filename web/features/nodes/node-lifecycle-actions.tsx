import { useState } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { TrashBin } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { clusterSnapshotKey } from '@/features/clusters/data'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { nodeQuery, nodeSnapshotQuery, removeNode } from './data'
import type { Node } from './types'

/** `删除节点` at the bottom of the config tab's basic group: a centered
 * confirmation states the consequence, then the page returns to the list.
 * Enabling and disabling stay the `启用状态` field, saved with the draft. */
export function NodeLifecycleActions({ node }: { node: Node }) {
  const client = useQueryClient()
  const navigate = useNavigate()
  const [removing, setRemoving] = useState(false)
  return (
    <>
      <Button
        size='sm'
        variant='danger-soft'
        className='self-start'
        onPress={() => setRemoving(true)}
      >
        <TrashBin />
        删除节点
      </Button>
      <ConfirmDialog
        isOpen={removing}
        onClose={() => setRemoving(false)}
        title={`删除节点 ${node.name}？`}
        description='节点将无法继续接入控制面，已分发的配置会在节点上停止更新。此操作不可撤销。'
        confirmLabel='删除节点'
        onConfirm={async () => {
          const response = await removeNode(node)
          toastWriteSuccess(response.message, response.data)
          // The node is gone, so an unsaved config draft is moot.
          await navigate({ to: '/nodes', replace: true, ignoreBlocker: true })
          client.removeQueries({ queryKey: nodeQuery(node.id).queryKey })
          client.removeQueries({
            queryKey: nodeSnapshotQuery(node.id).queryKey,
          })
          // The cluster detail header counts nodes.
          void client.invalidateQueries({ queryKey: clusterSnapshotKey })
        }}
      />
    </>
  )
}
