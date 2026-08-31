import type { PaginatedResponse, PaginatedResult } from './pagination.types';

export function normalizePaginatedResponse<T>(response: PaginatedResponse<T>): PaginatedResult<T> {
    const { page_size: pageSize, total_pages: totalPages, ...result } = response;
    return { ...result, pageSize, totalPages };
}
