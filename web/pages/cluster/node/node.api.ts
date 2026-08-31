import request, { type RevisionedResourceRef, withExpectedRevision } from '@/utils/http';
import { normalizePaginatedResponse } from '@/utils/pagination.response';
import type { PaginatedResponse } from '@/utils/pagination.types';
import { appendQueryParams } from '@/utils/query.params';
import type {
    NodeCredentialsInfo,
    NodeItem,
    NodeLogLimit,
    NodeQuery,
    NodeSaveInput,
    NodeWire,
} from './node.types';

const NODES_BASE = '/api/nodes';

function decorateNode(node: NodeWire): NodeItem {
    return {
        ...node,
        endpoints: node.config.endpoints,
        registration_status: node.runtime.registration_status,
        connection_status: node.runtime.connection_status,
        agent_version: node.runtime.agent_version,
        last_heartbeat_at: node.runtime.last_heartbeat_at,
        node_spec_revision: node.node_spec_revision,
        applied_node_spec_revision: node.runtime.applied_node_spec_revision,
        active_release_id: node.runtime.active_release_id,
        active_manifest_digest: node.runtime.active_manifest_digest,
    };
}

export async function getNodes(query?: NodeQuery) {
    const result = normalizePaginatedResponse(
        await request.get<PaginatedResponse<NodeWire>>(appendQueryParams(NODES_BASE, query))
    );
    return { ...result, list: result.list.map(decorateNode) };
}

export function createNode(input: NodeSaveInput) {
    return request.post<NodeCredentialsInfo>(NODES_BASE, input);
}

export function saveNode(target: RevisionedResourceRef, input: NodeSaveInput) {
    return request.putOperation(
        `${NODES_BASE}/${target.id}`,
        input,
        withExpectedRevision(target.revision)
    );
}

export function getNodeCredentials(id: string) {
    return request.get<NodeCredentialsInfo>(`${NODES_BASE}/${id}/credentials`);
}

export function getNodeLogStreamUrl(id: string, limit: NodeLogLimit, after?: string) {
    return appendQueryParams(`${NODES_BASE}/${id}/logs/stream`, { limit, after });
}

export function resetNodeCredentials(target: RevisionedResourceRef) {
    return request.post<NodeCredentialsInfo>(
        `${NODES_BASE}/${target.id}/credentials`,
        undefined,
        withExpectedRevision(target.revision)
    );
}

export function removeNode(target: RevisionedResourceRef) {
    return request.deleteOperation(
        `${NODES_BASE}/${target.id}`,
        withExpectedRevision(target.revision)
    );
}
