import type { ReactNode } from 'react';
import { cn } from '@/lib/utils';

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
        <dl
            className={cn(
                'grid overflow-hidden rounded-lg border bg-card',
                columns === 2 && 'sm:grid-cols-2',
                columns === 3 && 'sm:grid-cols-2 lg:grid-cols-3',
                className
            )}
        >
            {items.map((item) => (
                <div
                    key={item.key ?? `${String(item.label)}-${String(item.value)}`}
                    className="min-w-0 border-b p-4 last:border-b-0 sm:border-r"
                >
                    <dt className="text-xs font-medium text-muted-foreground">{item.label}</dt>
                    <dd className="mt-1 min-w-0 break-words text-sm text-foreground">
                        {item.value || '—'}
                    </dd>
                </div>
            ))}
        </dl>
    );
}
