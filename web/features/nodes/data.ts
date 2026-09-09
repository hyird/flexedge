import { z } from 'zod'
import { queryOptions } from '@tanstack/react-query'
import { sendData, streamPath, type PageData } from '@/lib/api'
import { readLiveQuery } from '@/lib/live-query'
import { ApiProtocolError } from '@/lib/api-response'
import { queryKeys } from '@/lib/query-keys'
import type { Node, NodeEndpoint } from './types'

const credentialsSchema = z.object({
  node_id: z.string().uuid(),
  secret: z.string().min(1),
  revision: z.number().int().positive(),
})

export type NodeCredentials = z.infer<typeof credentialsSchema>
export type NodeInput = {
  cluster_id: string
  name: string
  status: 'enabled' | 'disabled'
  config: { endpoints: NodeEndpoint[] }
}

export async function createNode(input: NodeInput) {
  const response = await sendData<unknown>('post', '/nodes/', input)
  return { ...response, data: parseCredentials(response.data) }
}

function parseCredentials(value: unknown): NodeCredentials {
  const result = credentialsSchema.safeParse(value)
  if (!result.success) throw new ApiProtocolError()
  return result.data
}

export async function getNodeCredentials(id: string) {
  const response = await sendData<unknown>('post', `/nodes/${id}/credentials/reveal`)
  return parseCredentials(response.data)
}

export async function updateNode(
  node: Pick<Node, 'id' | 'revision'>,
  input: NodeInput
) {
  const { code, message } = await sendData(
    'put',
    `/nodes/${node.id}`,
    input,
    node.revision
  )
  return { code, message, data: undefined }
}

export function nodesQuery(params: {
  page: number
  page_size: number
  keyword?: string
  cluster_id?: string
  status?: string
}) {
  return queryOptions({
  retry: false,
    queryKey: [...queryKeys.nodes, 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery<PageData<Node>>(
        streamPath('/nodes', params),
        queryKey,
        signal,
        undefined,
        (current, value) => {
          if (!current || !value || typeof value !== 'object') return current
          const patch = value as { id?: string; node_revision?: number; runtime?: Node['runtime'] }
          if (!patch.id || patch.node_revision === undefined || !patch.runtime) return current
          return {
            ...current,
            list: current.list.map((node) =>
              node.id === patch.id && node.revision === patch.node_revision &&
              !(node.runtime.last_heartbeat_at && patch.runtime?.last_heartbeat_at &&
                Date.parse(node.runtime.last_heartbeat_at) > Date.parse(patch.runtime.last_heartbeat_at))
                ? {
                    ...node,
                    runtime: {
                      ...node.runtime,
                      ...patch.runtime,
                      registration_status: node.runtime.registration_status,
                      connection_status: node.runtime.connection_status,
                    },
                  }
                : node
            ),
          }
        }
      ),
  })
}
export function removeNode(node: Pick<Node, 'id' | 'revision'>) {
  return sendData('delete', '/nodes/' + node.id, undefined, node.revision)
}
