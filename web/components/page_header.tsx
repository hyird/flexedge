import type { ReactNode } from 'react';
import { cn } from '@/lib/utils';

interface PageHeaderProps {
    title: string;
    description?: string;
    eyebrow?: string;
    actions?: ReactNode;
    className?: string;
}

export function PageHeader({ title, description, eyebrow, actions, className }: PageHeaderProps) {
    return (
        <header
            className={cn(
                'flex flex-col gap-4 sm:flex-row sm:items-start sm:justify-between',
                className
            )}
        >
            <div className="min-w-0">
                {eyebrow && (
                    <p className="mb-1 text-xs font-semibold uppercase tracking-[0.14em] text-primary">
                        {eyebrow}
                    </p>
                )}
                <h1 className="truncate text-xl font-semibold tracking-tight text-foreground sm:text-2xl">
                    {title}
                </h1>
                {description && (
                    <p className="mt-1 max-w-3xl text-sm text-muted-foreground">{description}</p>
                )}
            </div>
            {actions && <div className="flex shrink-0 flex-wrap items-center gap-2">{actions}</div>}
        </header>
    );
}
