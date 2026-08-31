import type { ComponentProps } from 'react';
import { cn } from '@/lib/utils';
export function Label({ className, ...props }: ComponentProps<'label'>) {
    return (
        // biome-ignore lint/a11y/noLabelWithoutControl: form controls associate this shared label through htmlFor at the call site.
        <label
            className={cn('ant-form-item-label block text-sm font-medium', className)}
            {...props}
        />
    );
}
