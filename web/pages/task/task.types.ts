import type { PageParams } from '@/utils/pagination.types';
import { createQueryKeys } from '@/utils/query.keys';

export type TaskStatus = 'pending' | 'running' | 'retry' | 'completed';

export type TaskIdentity =
    | { kind: 'provider'; resource_type: 'provider'; operation: 'verify' }
    | {
          kind: 'dns';
          resource_type: 'dns_zone';
          operation: 'sync' | 'sync_local' | 'sync_remote' | 'delete';
      }
    | { kind: 'certificate'; resource_type: 'certificate'; operation: 'issue' | 'renew' }
    | { kind: 'website'; resource_type: 'website'; operation: 'apply' | 'delete' };

export type TaskKind = TaskIdentity['kind'];
export type TaskResourceType = TaskIdentity['resource_type'];
export type TaskOperation = TaskIdentity['operation'];

export type TaskItem = TaskIdentity & {
    id: string;
    sequence: number;
    resource_id: string;
    resource_name: string;
    status: TaskStatus;
    version: number;
    processed_version: number;
    count_fails: number;
    next_attempt_at: string;
    lease_until?: string;
    error?: string;
    completed_at?: string;
    created_at: string;
    updated_at: string;
};

export interface TaskSummary {
    pending: number;
    running: number;
    retry: number;
    completed: number;
}

export interface TaskQuery extends PageParams {
    status?: TaskStatus;
    resourceType?: TaskResourceType;
    resourceId?: string;
}

export interface TaskPage {
    list: TaskItem[];
    summary: TaskSummary;
    total: number;
    page: number;
    page_size: number;
    total_pages: number;
}

export const taskQueryKeys = createQueryKeys('tasks');
