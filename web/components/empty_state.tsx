import { Empty } from 'antd';
import type { ReactNode } from 'react';
import { cn } from '@/lib/utils';

interface EmptyStateProps {
    title?: string;
    description?: string;
    icon?: ReactNode;
    action?: ReactNode;
    className?: string;
}

export function EmptyState({
    title = '暂无数据',
    description,
    icon,
    action,
    className,
}: EmptyStateProps) {
    return (
        <Empty
            className={cn(
                'flex min-h-44 flex-col items-center justify-center px-6 py-10',
                className
            )}
            image={icon}
            description={
                <span>
                    {title}
                    {description && <span className="block">{description}</span>}
                </span>
            }
        >
            {action}
        </Empty>
    );
}
