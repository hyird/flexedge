import { Dropdown } from 'antd';
import type { MenuProps } from 'antd';
import { Children, isValidElement, type ReactElement, type ReactNode } from 'react';

type MarkerProps = {
    children?: ReactNode;
    onSelect?: () => void;
    disabled?: boolean;
    variant?: 'default' | 'destructive';
    inset?: boolean;
    asChild?: boolean;
    checked?: boolean;
    value?: string;
    className?: string;
    title?: string;
};
function itemsFrom(children: ReactNode): NonNullable<MenuProps['items']> {
    const items: NonNullable<MenuProps['items']> = [];
    Children.forEach(children, (child, index) => {
        if (!isValidElement<MarkerProps>(child)) return;
        if (child.type === DropdownMenuSeparator)
            items.push({ type: 'divider', key: `divider-${index}` });
        else if (
            child.type === DropdownMenuItem ||
            child.type === DropdownMenuCheckboxItem ||
            child.type === DropdownMenuRadioItem
        )
            items.push({
                key: `item-${index}`,
                label: child.props.children,
                disabled: child.props.disabled,
                danger: child.props.variant === 'destructive',
                onClick: child.props.onSelect,
            });
        else items.push(...itemsFrom(child.props.children));
    });
    return items;
}
export function DropdownMenu({ children }: MarkerProps) {
    let trigger: ReactElement | undefined;
    let content: ReactNode;
    Children.forEach(children, (child) => {
        if (!isValidElement<MarkerProps>(child)) return;
        if (child.type === DropdownMenuTrigger) trigger = child.props.children as ReactElement;
        if (child.type === DropdownMenuContent) content = child.props.children;
    });
    return trigger ? (
        <Dropdown menu={{ items: itemsFrom(content) }} placement="bottomRight" trigger={['click']}>
            {trigger}
        </Dropdown>
    ) : (
        children
    );
}
export function DropdownMenuTrigger({ children }: MarkerProps) {
    return <>{children}</>;
}
export function DropdownMenuContent({ children }: MarkerProps & { align?: string }) {
    return <>{children}</>;
}
export function DropdownMenuItem(_props: MarkerProps) {
    return null;
}
export function DropdownMenuSeparator() {
    return null;
}
export function DropdownMenuCheckboxItem(_props: MarkerProps) {
    return null;
}
export function DropdownMenuRadioItem(_props: MarkerProps) {
    return null;
}
export function DropdownMenuGroup({ children }: MarkerProps) {
    return <>{children}</>;
}
export function DropdownMenuLabel({ children }: MarkerProps) {
    return <>{children}</>;
}
export function DropdownMenuPortal({ children }: MarkerProps) {
    return <>{children}</>;
}
export function DropdownMenuRadioGroup({ children }: MarkerProps) {
    return <>{children}</>;
}
export function DropdownMenuShortcut({ children }: MarkerProps) {
    return <>{children}</>;
}
export function DropdownMenuSub({ children }: MarkerProps) {
    return <>{children}</>;
}
export function DropdownMenuSubContent({ children }: MarkerProps) {
    return <>{children}</>;
}
export function DropdownMenuSubTrigger({ children }: MarkerProps) {
    return <>{children}</>;
}
