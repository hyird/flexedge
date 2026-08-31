import { Circle } from 'lucide-react';
import type { ComponentProps } from 'react';
import { Badge } from '@/components/ui/badge';

interface StatusBadgeProps extends Omit<ComponentProps<typeof Badge>, 'variant'> {
    tone?: 'success' | 'warning' | 'destructive' | 'info' | 'neutral';
    pulse?: boolean;
}

export function StatusBadge({
    tone = 'neutral',
    pulse = false,
    children,
    ...props
}: StatusBadgeProps) {
    const variant = tone === 'destructive' ? 'destructive' : tone;
    return (
        <Badge variant={variant} {...props}>
            <span className="relative flex size-2 items-center justify-center">
                {pulse && (
                    <span className="absolute size-2 animate-ping rounded-full bg-current opacity-40" />
                )}
                <Circle className="size-1.5 fill-current" />
            </span>
            {children}
        </Badge>
    );
}
