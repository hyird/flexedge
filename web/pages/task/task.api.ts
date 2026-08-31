import request from '@/utils/http';
import { normalizePaginatedResponse } from '@/utils/pagination.response';
import { appendQueryParams } from '@/utils/query.params';
import type { TaskPage, TaskQuery } from './task.types';

const BASE = '/api/tasks';

export async function getTasks(query: TaskQuery) {
    const response = await request.get<TaskPage>(appendQueryParams(BASE, query));
    return {
        ...normalizePaginatedResponse(response),
        summary: response.summary,
    };
}
