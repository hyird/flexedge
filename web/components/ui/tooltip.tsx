import { Tooltip as AntTooltip } from 'antd';
import { Children, isValidElement, type ReactElement, type ReactNode } from 'react';
export function TooltipProvider({ children }: { children?: ReactNode }) {
    return <>{children}</>;
}
export function TooltipContent({ children }: { children?: ReactNode; className?: string }) {
    return <>{children}</>;
}
export function TooltipTrigger({ children }: { children?: ReactNode; asChild?: boolean }) {
    return <>{children}</>;
}
export function Tooltip({ children }: { children?: ReactNode }) {
    let trigger: ReactElement | undefined;
    let title: ReactNode;
    Children.forEach(children, (child) => {
        if (!isValidElement<{ children?: ReactNode }>(child)) return;
        if (child.type === TooltipTrigger) trigger = child.props.children as ReactElement;
        else if (child.type === TooltipContent) title = child.props.children;
    });
    return trigger ? <AntTooltip title={title}>{trigger}</AntTooltip> : children;
}
