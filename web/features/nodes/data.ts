import { z } from 'zod'
import { queryOptions, type QueryClient } from '@tanstack/react-query'
import { sendData, streamPath, type PageData } from '@/lib/api'
import { ApiProtocolError } from '@/lib/api-response'
import { readLiveQuery, snapshotQueryOptions } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import type { Node, NodeEndpoint } from './types'

// service/domains/node/node_types.h NodeCredentialsDto. `node_id` is the agent
// id (32 lowercase hex, not the node UUID) and the secret is printable ASCII,
// matching the checks in node/install_node.sh.
const credentialsSchema = z.object({
  node_id: z.string().regex(/^[0-9a-f]{32}$/),
  secret: z
    .string()
    .min(32)
    .max(128)
    .regex(/^[!-~]+$/),
  revision: z.number().int().positive(),
  // Creation and reset start a task (operation-tasks contract §6).
  operation_id: z.string().optional(),
})

export type NodeCredentials = z.infer<typeof credentialsSchema>

// `POST /api/nodes` also returns the new node's UUID (`id`), so creation can
// continue straight to the node detail.
const createdNodeSchema = credentialsSchema.extend({ id: z.string().uuid() })
export type CreatedNode = z.infer<typeof createdNodeSchema>
export type NodeInput = {
  cluster_id: string
  name: string
  status: 'enabled' | 'disabled'
  config: { endpoints: NodeEndpoint[] }
}
type NodeConnectionStatus = 'unregistered' | 'online' | 'offline'
type NodesQueryParams = {
  page: number
  page_size: number
  keyword?: string
  cluster_id?: string
  status?: string
  connection_status?: NodeConnectionStatus
}

export async function createNode(input: NodeInput) {
  const response = await sendData<unknown>('post', '/nodes/', input)
  const result = createdNodeSchema.safeParse(response.data)
  if (!result.success) throw new ApiProtocolError()
  return { ...response, data: result.data }
}

function parseCredentials(value: unknown): NodeCredentials {
  const result = credentialsSchema.safeParse(value)
  if (!result.success) throw new ApiProtocolError()
  return result.data
}

export async function getNodeCredentials(id: string) {
  const response = await sendData<unknown>(
    'post',
    `/nodes/${id}/credentials/reveal`
  )
  return parseCredentials(response.data)
}

/** Issues a new agent id and secret; the old credentials stop working and
 * the node returns to `pending` registration. */
export async function resetNodeCredentials(
  node: Pick<Node, 'id' | 'revision'>
) {
  const response = await sendData<unknown>(
    'post',
    `/nodes/${node.id}/credentials`,
    undefined,
    node.revision
  )
  return { message: response.message, data: parseCredentials(response.data) }
}

const updatedNodeSchema = z.object({ operation_id: z.string() })

/** Only the started task (`operation_id`) of the update response is kept;
 * anything else in `data` is never exposed. */
export async function updateNode(
  node: Pick<Node, 'id' | 'revision'>,
  input: NodeInput
) {
  const { code, message, data } = await sendData(
    'put',
    `/nodes/${node.id}`,
    input,
    node.revision
  )
  const operation = updatedNodeSchema.safeParse(data)
  return {
    code,
    message,
    data: operation.success
      ? { operation_id: operation.data.operation_id }
      : undefined,
  }
}

export function removeNode(node: Pick<Node, 'id' | 'revision'>) {
  return sendData('delete', '/nodes/' + node.id, undefined, node.revision)
}

/** Applies a `node-runtime` event to one node. Only the node's current
 * revision is patched, heartbeats never move backwards and registration and
 * connection states stay owned by snapshots. Returns `node` unchanged when the
 * event does not apply. */
function applyNodeRuntimePatch(node: Node, value: unknown): Node {
  if (!value || typeof value !== 'object') return node
  const patch = value as {
    id?: string
    node_revision?: number
    runtime?: Node['runtime']
  }
  if (
    !patch.id ||
    patch.node_revision === undefined ||
    !patch.runtime ||
    node.id !== patch.id ||
    node.revision !== patch.node_revision
  )
    return node
  const current = node.runtime.last_heartbeat_at,
    next = patch.runtime.last_heartbeat_at
  if (current && next && Date.parse(current) > Date.parse(next)) return node
  return {
    ...node,
    runtime: {
      ...node.runtime,
      ...patch.runtime,
      registration_status: node.runtime.registration_status,
      connection_status: node.runtime.connection_status,
    },
  }
}

/** Applies a `node-runtime` event to a node page (list stream, overview
 * `nodes` event). Returns `current` unchanged when no node matches. */
export function applyNodeRuntimePagePatch(
  current: PageData<Node> | undefined,
  value: unknown
): PageData<Node> | undefined {
  if (!current) return current
  let changed = false
  const list = current.list.map((node) => {
    const next = applyNodeRuntimePatch(node, value)
    if (next !== node) changed = true
    return next
  })
  return changed ? { ...current, list } : current
}

/** Node list stream with `node-runtime` patches. */
export function nodesQuery(params: NodesQueryParams) {
  return queryOptions({
    retry: false,
    queryKey: [...queryKeys.nodes, 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery<PageData<Node>>(
        streamPath('/nodes', params),
        queryKey,
        signal,
        undefined,
        applyNodeRuntimePagePatch
      ),
  })
}

/** Which nodes a list snapshot shows and their online state. Runtime patches
 * never change it (they keep `connection_status`), so it only moves when a
 * snapshot reports an online/offline transition or nodes were added or
 * removed. */
function nodeConnectionSignature(page: PageData<Node>) {
  return [
    page.total,
    ...page.list.map((node) => `${node.id}:${node.runtime.connection_status}`),
  ].join('|')
}

/** Calls `onTransition` once per burst (coalesced over `delayMs`) when a
 * cached node list of the given cluster changes which nodes it holds or their
 * online state. Driven by the list's own stream; it reads nothing itself, so
 * a page with this list live can refresh one-shot counters (the cluster
 * header) without a second stream. Returns the unsubscribe function. */
export function watchClusterNodeTransitions(
  client: QueryClient,
  clusterId: string,
  onTransition: () => void,
  delayMs = 300
) {
  const signatures = new Map<string, string>()
  let timer: ReturnType<typeof setTimeout> | undefined
  const unsubscribe = client.getQueryCache().subscribe((event) => {
    const [scope, kind, params] = event.query.queryKey as [
      unknown,
      unknown,
      NodesQueryParams | undefined,
    ]
    if (
      scope !== queryKeys.nodes[0] ||
      kind !== 'list' ||
      params?.cluster_id !== clusterId
    )
      return
    if (event.type === 'removed') {
      signatures.delete(event.query.queryHash)
      return
    }
    if (event.type !== 'updated' || event.action.type !== 'success') return
    const data = event.query.state.data as PageData<Node> | undefined
    if (!data) return
    const next = nodeConnectionSignature(data)
    const previous = signatures.get(event.query.queryHash)
    signatures.set(event.query.queryHash, next)
    if (previous === undefined || previous === next || timer !== undefined)
      return
    timer = setTimeout(() => {
      timer = undefined
      onTransition()
    }, delayMs)
  })
  return () => {
    clearTimeout(timer)
    unsubscribe()
  }
}

/** Single node stream (`/api/nodes/:id/stream`): the same DTO as list items,
 * kept current by snapshots and `node-runtime` patches. The node detail's
 * overview and config tabs share it (header included). */
export function nodeQuery(id: string) {
  return queryOptions({
    retry: false,
    meta: { inlineError: true },
    queryKey: [...queryKeys.nodes, 'detail', id],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery<Node>(
        streamPath('/nodes/' + id),
        queryKey,
        signal,
        undefined,
        (current, value) =>
          current ? applyNodeRuntimePatch(current, value) : current
      ),
  })
}

export const nodeSnapshotKey = [...queryKeys.nodes, 'snapshot'] as const

/** One read of the node detail stream, closed after the first frame. Used by
 * the detail header while the logs tab owns the page's only live stream;
 * writes on the page re-read it by invalidating `nodeSnapshotKey`. The
 * layout keeps a disabled observer on other tabs, so the cached read is
 * stale at once and every logs visit reads again. */
export function nodeSnapshotQuery(id: string) {
  return {
    ...snapshotQueryOptions<Node>(streamPath('/nodes/' + id), [
      ...nodeSnapshotKey,
      id,
    ]),
    staleTime: 0,
    meta: { inlineError: true },
  }
}
