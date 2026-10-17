import { z } from 'zod'
import { queryOptions, type QueryClient } from '@tanstack/react-query'
import {
  getData,
  pageDataSchema,
  sendData,
  streamPath,
  type PageData,
} from '@/lib/api'
import { parseResponseData } from '@/lib/api-response'
import { readLiveQuery } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import { resourceListSearchQuery } from '@/lib/resource'
import { clusterSnapshotKey } from '@/features/clusters/data'
import { liveLogsQuery } from '@/features/logs/data'
import { parseNodeLogs } from './node-log-schema'
import {
  nodeRuntimePatchSchema,
  nodeSchema,
  type Node,
  type NodeEndpoint,
} from './types'

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
})

export type NodeCredentials = z.infer<typeof credentialsSchema>

// `POST /api/nodes` also returns the new node's UUID (`id`), so creation can
// continue straight to the node detail.
const createdNodeSchema = credentialsSchema.extend({ id: z.string().uuid() })
export type CreatedNode = z.infer<typeof createdNodeSchema>
export type NodeInput = {
  cluster_id: string
  name: string
  status: Node['status']
  config: { endpoints: NodeEndpoint[] }
}
type NodesQueryParams = {
  page: number
  page_size: number
  keyword?: string
  cluster_id?: string
  status?: Node['status']
  connection_status?: Node['runtime']['connection_status']
}

const nodePageSchema = pageDataSchema(nodeSchema)

export async function createNode(input: NodeInput) {
  const response = await sendData<unknown>('post', '/nodes/', input)
  return {
    ...response,
    data: parseResponseData(createdNodeSchema, response.data),
  }
}

function parseCredentials(value: unknown): NodeCredentials {
  return parseResponseData(credentialsSchema, value)
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

/** Update acknowledgement and the revision it produced; anything in `data`
 * is never exposed. */
export async function updateNode(
  node: Pick<Node, 'id' | 'revision'>,
  input: NodeInput
) {
  const { code, message, revision } = await sendData(
    'put',
    `/nodes/${node.id}`,
    input,
    node.revision
  )
  return { code, message, revision }
}

export function removeNode(node: Pick<Node, 'id' | 'revision'>) {
  return sendData('delete', '/nodes/' + node.id, undefined, node.revision)
}

type NodeRuntimePatch = z.infer<typeof nodeRuntimePatchSchema>

/** Applies a parsed `node-runtime` event to one node. Only the node's current
 * revision is patched, heartbeats never move backwards and registration and
 * connection states stay owned by snapshots. Returns `node` unchanged when the
 * event does not apply. */
function applyNodeRuntimePatch(node: Node, patch: NodeRuntimePatch): Node {
  if (node.id !== patch.id || node.revision !== patch.node_revision) return node
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
  const patch = parseResponseData(nodeRuntimePatchSchema, value)
  let changed = false
  const list = current.list.map((node) => {
    const next = applyNodeRuntimePatch(node, patch)
    if (next !== node) changed = true
    return next
  })
  return changed ? { ...current, list } : current
}

/** Node list stream with `node-runtime` patches. */
export function nodesQuery(params: NodesQueryParams) {
  return queryOptions({
    retry: false,
    meta: { inlineError: true },
    queryKey: [...queryKeys.nodes, 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        streamPath('/nodes', params),
        queryKey,
        signal,
        (value) => parseResponseData(nodePageSchema, value),
        applyNodeRuntimePagePatch
      ),
  })
}

/** Command palette search of nodes. */
export function nodeSearchQuery(keyword: string) {
  return resourceListSearchQuery(
    '/nodes',
    queryKeys.nodes,
    keyword,
    nodeSchema.pick({ id: true, name: true }),
    (node) => ({ id: node.id, label: node.name })
  )
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
      readLiveQuery(
        streamPath('/nodes/' + id),
        queryKey,
        signal,
        (value) => parseResponseData(nodeSchema, value),
        (current, value) =>
          current
            ? applyNodeRuntimePatch(
                current,
                parseResponseData(nodeRuntimePatchSchema, value)
              )
            : current
      ),
  })
}

export const nodeSnapshotKey = [...queryKeys.nodes, 'snapshot'] as const

/** One read of `GET /api/nodes/:id`. Used by the detail header while the
 * logs tab owns the page's live stream; writes on the page re-read it by
 * `afterNodeWrite`. The layout keeps a disabled observer on
 * other tabs, so the cached read is stale at once and every logs visit
 * reads again. */
export function nodeSnapshotQuery(id: string) {
  return queryOptions({
    retry: false,
    gcTime: 0,
    staleTime: 0,
    meta: { inlineError: true },
    queryKey: [...nodeSnapshotKey, id],
    queryFn: async ({ signal }) =>
      parseResponseData(
        nodeSchema,
        await getData<unknown>('/nodes/' + id, undefined, signal)
      ),
  })
}

/** After a node write that keeps the node (credentials reset, config save):
 * header snapshots (logs tab) re-read, and the cluster detail header, which
 * counts nodes and their online state. Live node streams push the change. */
export function afterNodeWrite(client: QueryClient) {
  void client.invalidateQueries({ queryKey: nodeSnapshotKey })
  void client.invalidateQueries({ queryKey: clusterSnapshotKey })
}

/** After creating a node: the cluster detail header counts nodes. */
export function afterNodeCreate(client: QueryClient) {
  void client.invalidateQueries({ queryKey: clusterSnapshotKey })
}

/** After deleting a node: its detail reads are dropped and the cluster
 * detail header re-reads its counts. */
export function afterNodeRemove(client: QueryClient, id: string) {
  client.removeQueries({ queryKey: nodeQuery(id).queryKey })
  client.removeQueries({ queryKey: nodeSnapshotQuery(id).queryKey })
  void client.invalidateQueries({ queryKey: clusterSnapshotKey })
}

/** Live node log tail: the 100 newest records, then each new batch. */
export function nodeLogsQuery(id: string) {
  return liveLogsQuery(
    streamPath('/nodes/' + id + '/logs', { limit: 100 }),
    [...queryKeys.nodes, 'logs', id],
    parseNodeLogs
  )
}
