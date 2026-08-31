import { Button as AntButton } from 'antd';
import type { ButtonHTMLType, ButtonProps as AntButtonProps } from 'antd/es/button';
import { cloneElement, isValidElement, type ReactElement, type ReactNode } from 'react';
import { cn } from '@/lib/utils';

type Variant = 'default' | 'outline' | 'secondary' | 'ghost' | 'destructive' | 'link';
type Size = 'default' | 'xs' | 'sm' | 'lg' | 'icon' | 'icon-sm' | 'icon-xs' | 'icon-lg';
export interface ButtonProps
    extends Omit<AntButtonProps, 'type' | 'size' | 'children' | 'variant' | 'htmlType'> {
    variant?: Variant;
    size?: Size;
    asChild?: boolean;
    type?: ButtonHTMLType;
    children?: ReactNode;
}
const sizes: Record<Size, AntButtonProps['size']> = {
    default: 'middle',
    xs: 'small',
    sm: 'small',
    lg: 'large',
    icon: 'middle',
    'icon-sm': 'small',
    'icon-xs': 'small',
    'icon-lg': 'large',
};
export function Button({
    variant = 'default',
    size = 'default',
    asChild,
    className,
    children,
    danger,
    type: htmlType = 'button',
    ...props
}: ButtonProps) {
    if (asChild && isValidElement(children))
        return cloneElement(children as ReactElement<{ className?: string }>, {
            className: cn((children.props as { className?: string }).className, className),
        });
    const antType: AntButtonProps['type'] =
        variant === 'link'
            ? 'link'
            : variant === 'ghost'
              ? 'text'
              : variant === 'default'
                ? 'primary'
                : 'default';
    return (
        <AntButton
            type={antType}
            htmlType={htmlType}
            danger={danger ?? variant === 'destructive'}
            size={sizes[size]}
            className={cn(
                '[&_svg]:size-4',
                size.startsWith('icon') && 'ant-btn-icon-only',
                className
            )}
            {...props}
        >
            {children}
        </AntButton>
    );
}
export const buttonVariants = () => '';
