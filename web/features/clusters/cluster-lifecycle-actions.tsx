import { useState } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { TrashBin } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
import { queryKeys } from '@/lib/query-keys'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { clusterSnapshotKey, removeCluster } from './data'
import type { Cluster } from './types'

/** Header action next to 返回: a centered
 * confirmation states the consequence, then the page returns to the list. */
export function ClusterLifecycleActions({ cluster }: { cluster: Cluster }) {
  const client = useQueryClient()
  const navigate = useNavigate()
  const [removing, setRemoving] = useState(false)
  return (
    <>
      <Button size='sm' variant='danger-soft' onPress={() => setRemoving(true)}>
        <TrashBin />
        删除集群
      </Button>
      <ConfirmDialog
        isOpen={removing}
        onClose={() => setRemoving(false)}
        title={`删除集群 ${cluster.name}？`}
        description={
          cluster.node_count > 0
            ? `集群下还有 ${cluster.node_count} 个节点，需先删除或迁移这些节点和集群内的网站后才能删除集群。`
            : '集群的接入域名将不再解析，此操作不可撤销。集群内仍有网站时无法删除。'
        }
        confirmLabel='删除集群'
        onConfirm={async () => {
          const response = await removeCluster(cluster)
          toastWriteSuccess(response.message, response.data)
          // The cluster is gone, so an unsaved configuration draft is moot.
          await navigate({
            to: '/clusters',
            replace: true,
            ignoreBlocker: true,
          })
          client.removeQueries({
            queryKey: [...clusterSnapshotKey, cluster.id],
          })
          void client.invalidateQueries({ queryKey: queryKeys.clusters })
        }}
      />
    </>
  )
}
