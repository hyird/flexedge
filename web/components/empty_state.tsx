import { Inbox } from 'lucide-react';
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
    icon = <Inbox className="size-5" />,
    action,
    className,
}: EmptyStateProps) {
    return (
        <div
            className={cn(
                'flex min-h-44 flex-col items-center justify-center px-6 py-10 text-center',
                className
            )}
        >
            <div className="mb-3 flex size-10 items-center justify-center rounded-xl border bg-muted/50 text-muted-foreground">
                {icon}
            </div>
            <p className="text-sm font-medium">{title}</p>
            {description && (
                <p className="mt-1 max-w-sm text-sm text-muted-foreground">{description}</p>
            )}
            {action && <div className="mt-4">{action}</div>}
        </div>
    );
}
