import { Tag } from 'antd';
import type { ComponentProps } from 'react';
type Variant =
    | 'default'
    | 'secondary'
    | 'destructive'
    | 'outline'
    | 'success'
    | 'warning'
    | 'info'
    | 'neutral';
const colors: Record<Variant, string | undefined> = {
    default: undefined,
    secondary: 'default',
    destructive: 'red',
    outline: undefined,
    success: 'green',
    warning: 'gold',
    info: 'blue',
    neutral: 'default',
};
export function Badge({
    variant = 'default',
    asChild: _asChild,
    ...props
}: Omit<ComponentProps<typeof Tag>, 'variant'> & { variant?: Variant; asChild?: boolean }) {
    return <Tag color={colors[variant]} {...props} />;
}
export const badgeVariants = () => '';
