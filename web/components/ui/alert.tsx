import { Alert as AntAlert } from 'antd';
import { Children, isValidElement, type ComponentProps, type ReactNode } from 'react';
export function Alert({
    variant,
    children,
    ...props
}: Omit<ComponentProps<typeof AntAlert>, 'message' | 'description' | 'variant'> & {
    variant?: 'default' | 'destructive';
    children?: ReactNode;
}) {
    let message: ReactNode;
    let description: ReactNode;
    Children.forEach(children, (child) => {
        if (!isValidElement<{ children?: ReactNode }>(child)) return;
        if (child.type === AlertTitle) message = child.props.children;
        else if (child.type === AlertDescription) description = child.props.children;
    });
    return (
        <AntAlert
            showIcon
            type={variant === 'destructive' ? 'error' : 'info'}
            message={message}
            description={description}
            {...props}
        />
    );
}
export function AlertTitle({ children }: ComponentProps<'div'>) {
    return <>{children}</>;
}
export function AlertDescription({ children }: ComponentProps<'div'>) {
    return <>{children}</>;
}
