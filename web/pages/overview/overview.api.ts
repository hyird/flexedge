import request from '@/utils/http';
import type { OverviewData } from './overview.types';

const BASE = '/api/overview';

export function getOverview() {
    return request.get<OverviewData>(BASE);
}
