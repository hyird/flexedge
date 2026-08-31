import { Card as AntCard } from 'antd';
import type { ComponentProps } from 'react';
import { cn } from '@/lib/utils';
export function Card({
    className,
    size,
    ...props
}: Omit<ComponentProps<typeof AntCard>, 'size'> & { size?: 'default' | 'sm' | 'small' }) {
    return (
        <AntCard
            size={size === 'small' || size === 'sm' ? 'small' : 'default'}
            className={cn('overflow-hidden', className)}
            {...props}
        />
    );
}
export function CardHeader({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('border-b px-6 py-4', className)} {...props} />;
}
export function CardTitle({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('text-base font-semibold', className)} {...props} />;
}
export function CardDescription({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('mt-1 text-sm text-gray-500', className)} {...props} />;
}
export function CardAction({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('ml-auto', className)} {...props} />;
}
export function CardContent({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('px-6 py-5', className)} {...props} />;
}
export function CardFooter({ className, ...props }: ComponentProps<'div'>) {
    return <div className={cn('flex items-center border-t px-6 py-4', className)} {...props} />;
}
