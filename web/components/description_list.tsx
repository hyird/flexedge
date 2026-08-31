import { Descriptions } from 'antd';
import type { ReactNode } from 'react';

export interface DescriptionItem {
    key?: string;
    label: ReactNode;
    value: ReactNode;
}

interface DescriptionListProps {
    items: DescriptionItem[];
    columns?: 1 | 2 | 3;
    className?: string;
}

export function DescriptionList({ items, columns = 2, className }: DescriptionListProps) {
    return (
        <Descriptions
            className={className}
            column={{ xs: 1, sm: Math.min(columns, 2), lg: columns }}
            items={items.map((item) => ({
                key: item.key ?? `${String(item.label)}-${String(item.value)}`,
                label: item.label,
                children: item.value || '—',
            }))}
        />
    );
}
