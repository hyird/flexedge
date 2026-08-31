import { useQuery } from '@tanstack/react-query';
import { getOverview } from './overview.api';
import { overviewQueryKeys } from './overview.types';

export function useOverview() {
    return useQuery({
        queryKey: overviewQueryKeys.all,
        queryFn: getOverview,
        staleTime: 0,
        refetchOnMount: 'always',
        refetchInterval: 30_000,
    });
}
