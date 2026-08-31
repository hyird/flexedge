import { Typography } from 'antd';
import type { ReactNode } from 'react';
import { cn } from '@/lib/utils';

const { Title } = Typography;

interface PageHeaderProps {
    title: string;
    description?: string;
    eyebrow?: string;
    actions?: ReactNode;
    className?: string;
}

export function PageHeader({ title, actions, className }: PageHeaderProps) {
    return (
        <header
            className={cn(
                'flex flex-col gap-4 sm:flex-row sm:items-start sm:justify-between',
                className
            )}
        >
            <div className="min-w-0">
                <Title level={5} className="truncate" style={{ margin: 0 }}>
                    {title}
                </Title>
            </div>
            {actions && <div className="flex shrink-0 flex-wrap items-center gap-2">{actions}</div>}
        </header>
    );
}
