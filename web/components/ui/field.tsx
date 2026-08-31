import type * as React from 'react';
import { cn } from '@/lib/utils';
import { Label } from './label';

function Field({ className, ...props }: React.ComponentProps<'div'>) {
    return <div data-slot="field" className={cn('grid gap-2', className)} {...props} />;
}

function FieldLabel({ className, ...props }: React.ComponentProps<typeof Label>) {
    return <Label data-slot="field-label" className={className} {...props} />;
}

function FieldDescription({ className, ...props }: React.ComponentProps<'p'>) {
    return (
        <p
            data-slot="field-description"
            className={cn('text-xs leading-relaxed text-muted-foreground', className)}
            {...props}
        />
    );
}

function FieldError({ className, children, ...props }: React.ComponentProps<'p'>) {
    if (!children) return null;
    return (
        <p
            data-slot="field-error"
            role="alert"
            className={cn('text-xs font-medium text-destructive', className)}
            {...props}
        >
            {children}
        </p>
    );
}

function FieldGroup({ className, ...props }: React.ComponentProps<'div'>) {
    return <div data-slot="field-group" className={cn('grid gap-4', className)} {...props} />;
}

export { Field, FieldDescription, FieldError, FieldGroup, FieldLabel };
