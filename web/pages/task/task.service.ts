import { useQuery } from '@tanstack/react-query';
import { getTasks } from './task.api';
import { type TaskQuery, type TaskSummary, taskQueryKeys } from './task.types';

const activeStatuses = new Set(['pending', 'running', 'retry']);

function hasActiveSync(data: { summary: TaskSummary }) {
    const { pending, running, retry } = data.summary;
    return pending + running + retry > 0;
}

export function useTaskList(query: TaskQuery, enabled = true, refetchInterval = 5000) {
    return useQuery({
        queryKey: taskQueryKeys.list({ ...query }),
        queryFn: () => getTasks(query),
        enabled,
        refetchInterval: (state) =>
            state.state.data && hasActiveSync(state.state.data) ? refetchInterval : false,
    });
}

export function isTaskActive(status: string) {
    return activeStatuses.has(status);
}
