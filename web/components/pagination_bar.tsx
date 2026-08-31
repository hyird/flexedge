import { Pagination } from 'antd';

interface PaginationBarProps {
    page: number;
    pageSize: number;
    total: number;
    onPageChange: (page: number) => void;
    onPageSizeChange?: (pageSize: number) => void;
    pageSizeOptions?: number[];
}

export function PaginationBar({
    page,
    pageSize,
    total,
    onPageChange,
    onPageSizeChange,
    pageSizeOptions = [10, 20, 50],
}: PaginationBarProps) {
    return (
        <div className="flex shrink-0 justify-end px-4 py-3">
            <Pagination
                current={page}
                pageSize={pageSize}
                total={total}
                showSizeChanger={Boolean(onPageSizeChange)}
                pageSizeOptions={pageSizeOptions}
                showTotal={(count, range) => `${range[0]}–${range[1]} / 共 ${count} 条`}
                onChange={(nextPage, nextPageSize) => {
                    if (nextPageSize !== pageSize) onPageSizeChange?.(nextPageSize);
                    else onPageChange(nextPage);
                }}
            />
        </div>
    );
}
