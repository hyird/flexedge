import { Empty, Table } from 'antd';
import type { ColumnsType } from 'antd/es/table';
import { type ReactNode, useEffect, useMemo, useState } from 'react';
import { cn } from '@/lib/utils';

export interface DataTableColumn<T> {
    key: string;
    header: ReactNode;
    cell: (row: T, index: number) => ReactNode;
    className?: string;
    headerClassName?: string;
}
interface DataTableProps<T> {
    columns: DataTableColumn<T>[];
    data?: T[];
    getRowKey: (row: T) => string;
    loading?: boolean;
    emptyTitle?: string;
    emptyDescription?: string;
    className?: string;
    tableClassName?: string;
    expandedRowKey?: string;
    renderExpanded?: (row: T) => ReactNode;
    onExpandedChange?: (row: T) => void;
}

export function DataTable<T extends object>({
    columns,
    data = [],
    getRowKey,
    loading = false,
    emptyTitle = '暂无数据',
    emptyDescription,
    className,
    tableClassName,
    expandedRowKey,
    renderExpanded,
    onExpandedChange,
}: DataTableProps<T>) {
    const [container, setContainer] = useState<HTMLDivElement | null>(null);
    const [headerRow, setHeaderRow] = useState<HTMLTableRowElement | null>(null);
    const [hasHorizontalOverflow, setHasHorizontalOverflow] = useState(false);
    useEffect(() => {
        if (!container || !headerRow) return;
        const updateOverflow = () => {
            setHasHorizontalOverflow(headerRow.scrollWidth > container.clientWidth + 1);
        };
        const observer = new ResizeObserver(updateOverflow);
        observer.observe(container);
        observer.observe(headerRow);
        updateOverflow();
        return () => observer.disconnect();
    }, [container, headerRow]);
    const antColumns = useMemo<ColumnsType<T>>(
        () =>
            columns.map((column) => ({
                key: column.key,
                title: <span className={column.headerClassName}>{column.header}</span>,
                render: (_value: unknown, record: T, index: number) => column.cell(record, index),
                className: cn(column.className, column.key === 'actions' && 'text-center'),
                align: column.key === 'actions' ? 'center' : undefined,
                width: column.key === 'actions' ? 72 : undefined,
                fixed: column.key === 'actions' && hasHorizontalOverflow ? 'right' : undefined,
            })),
        [columns, hasHorizontalOverflow]
    );
    const captureHeaderRow = () => ({ ref: setHeaderRow }) as never;
    return (
        <div
            ref={setContainer}
            className={cn('ant-data-table h-full min-w-0 overflow-hidden', className)}
        >
            <Table<T>
                rowKey={getRowKey}
                columns={antColumns}
                dataSource={data}
                loading={loading}
                pagination={false}
                sticky
                onHeaderRow={captureHeaderRow}
                scroll={{ x: 'max-content', y: '100%' }}
                className={tableClassName}
                locale={{
                    emptyText: (
                        <Empty
                            description={
                                <div>
                                    <div>{emptyTitle}</div>
                                    {emptyDescription && (
                                        <div className="mt-1 text-xs text-gray-400">
                                            {emptyDescription}
                                        </div>
                                    )}
                                </div>
                            }
                        />
                    ),
                }}
                expandable={
                    renderExpanded
                        ? {
                              expandedRowKeys: expandedRowKey ? [expandedRowKey] : [],
                              expandedRowRender: renderExpanded,
                              onExpand: (_expanded, record) => onExpandedChange?.(record),
                              columnWidth: 48,
                          }
                        : undefined
                }
            />
        </div>
    );
}
