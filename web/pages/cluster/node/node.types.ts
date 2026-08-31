import type { LogTail, LogTailLimit } from '@/utils/log_tail';
import type { RevisionedResourceRef } from '@/utils/http';
import type { PageParams } from '@/utils/pagination.types';
import { createQueryKeys } from '@/utils/query.keys';

export type NodeStatus = 'enabled' | 'disabled';
export type NodeRegistrationStatus = 'pending' | 'registered';
export type NodeConnectionStatus = 'unregistered' | 'online' | 'offline';

export interface NodeEndpoint {
    id?: string;
    ip_address: string;
    line_code: string;
    line_name?: string;
}

export interface NodeWire {
    id: string;
    cluster_id: string;
    cluster_name: string;
    name: string;
    status: NodeStatus;
    node_spec_revision: number;
    created_at: string;
    updated_at: string;
    revision: number;
    config: NodeConfig;
    runtime: NodeRuntime;
}

export interface NodeItem extends NodeWire {
    endpoints: NodeEndpoint[];
    registration_status: NodeRegistrationStatus;
    connection_status: NodeConnectionStatus;
    agent_version?: string;
    last_heartbeat_at?: string;
    applied_node_spec_revision: number;
    active_release_id?: string;
    active_manifest_digest?: string;
}

export interface NodeConfig {
    endpoints: Array<Required<Pick<NodeEndpoint, 'id' | 'ip_address' | 'line_code'>>>;
}

export interface NodeRuntime {
    registration_status: NodeRegistrationStatus;
    connection_status: NodeConnectionStatus;
    agent_version?: string;
    last_heartbeat_at?: string;
    applied_node_spec_revision: number;
    active_release_id?: string;
    active_manifest_digest?: string;
    cpu_usage?: number;
    memory_usage?: number;
    traffic_out_bps?: number;
    connection_count?: number;
    load_1m?: number;
    queued_log_events?: number;
    dropped_log_events?: number;
    health?: string;
    last_error?: string;
}

export interface NodeSaveInput {
    cluster_id: string;
    name: string;
    status: NodeStatus;
    config: NodeConfig;
}

export interface NodeQuery extends PageParams {
    clusterId?: string;
    status?: NodeStatus;
    registrationStatus?: NodeRegistrationStatus;
    connectionStatus?: NodeConnectionStatus;
}

export interface NodeSaveCommand {
    target?: RevisionedResourceRef;
    input: NodeSaveInput;
}

export interface NodeCredentialsInfo {
    revision: number;
    node_id: string;
    secret: string;
}

export interface NodeLog {
    id: string;
    occurred_at: string;
    level: 'info' | 'warning' | 'error';
    category: string;
    message: string;
}

export type NodeLogLimit = LogTailLimit;
export type NodeLogTail = LogTail<NodeLog>;

export const nodeQueryKeys = createQueryKeys('nodes');
