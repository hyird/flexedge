import { z } from 'zod'
import { revisionedSchema } from '@/lib/resource'
import { statusIds } from '@/lib/status'
import { connectionStatuses, nodeStatuses } from './node-display'

// service/features/node_config/transport.h node_endpoint_output.
export const nodeEndpointSchema = z.object({
  id: z.string(),
  ip_address: z.string(),
  line_code: z.string(),
})
export type NodeEndpoint = z.infer<typeof nodeEndpointSchema>

// service/domains/node/node_types.h node_runtime_dto, also the `runtime` of a
// `node-runtime` patch (service/features/node_channel/runtime_mapper.h).
// RUVIA_OMIT_EMPTY fields are omitted, never null. `registration_status`
// (pending/registered) and the agent-reported `health` stay strings; the
// display falls back to their raw text.
export const nodeRuntimeSchema = z.object({
  registration_status: z.string(),
  connection_status: z.enum(statusIds(connectionStatuses)),
  last_heartbeat_at: z.string().optional(),
  applied_node_spec_revision: z.number().int(),
  active_release_id: z.string().optional(),
  active_manifest_digest: z.string().optional(),
  agent_version: z.string().optional(),
  cpu_usage: z.number().optional(),
  memory_usage: z.number().optional(),
  cpu_count: z.number().int().positive().optional(),
  memory_total_bytes: z.number().int().positive().optional(),
  disk_total_bytes: z.number().int().positive().optional(),
  traffic_out_bps: z.number().int().optional(),
  connection_count: z.number().int().optional(),
  connected_ip_count: z.number().int().optional(),
  load_1m: z.number().optional(),
  queued_log_events: z.number().int().optional(),
  dropped_log_events: z.number().int().optional(),
  health: z.string().optional(),
  last_error: z.string().optional(),
})

// service/domains/node/node_types.h node_dto: list items, detail and stream
// snapshots share it. `status` is validated as enabled/disabled on save.
export const nodeSchema = revisionedSchema.extend({
  cluster_id: z.string(),
  cluster_name: z.string(),
  name: z.string(),
  status: z.enum(statusIds(nodeStatuses)),
  node_spec_revision: z.number().int(),
  config: z.object({ endpoints: z.array(nodeEndpointSchema) }),
  runtime: nodeRuntimeSchema,
})
export type Node = z.infer<typeof nodeSchema>

// service/features/node_channel/runtime_mapper.h node_runtime_patch.
export const nodeRuntimePatchSchema = z.object({
  id: z.string(),
  node_revision: z.number().int(),
  runtime: nodeRuntimeSchema,
})
