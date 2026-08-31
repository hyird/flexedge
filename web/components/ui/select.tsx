import { Select as AntSelect, Divider } from 'antd';
import type { SelectProps } from 'antd';
import { Children, isValidElement, type ReactNode } from 'react';

type ValueChangeHandler = { bivarianceHack(value: string): void }['bivarianceHack'];
type RootProps = Omit<SelectProps, 'onChange' | 'options' | 'size'> & {
    onValueChange?: ValueChangeHandler;
    children?: ReactNode;
    size?: 'sm' | 'default';
};
type MarkerProps = {
    children?: ReactNode;
    value?: string;
    disabled?: boolean;
    className?: string;
    placeholder?: ReactNode;
    size?: 'sm' | 'default';
    id?: string;
};
function collect(
    children: ReactNode,
    state: {
        options: NonNullable<SelectProps['options']>;
        placeholder?: ReactNode;
        className?: string;
        id?: string;
    }
) {
    Children.forEach(children, (child) => {
        if (!isValidElement<MarkerProps>(child)) return;
        if (child.type === SelectItem && child.props.value)
            state.options.push({
                value: child.props.value,
                label: child.props.children,
                disabled: child.props.disabled,
            });
        if (child.type === SelectValue) state.placeholder = child.props.placeholder;
        if (child.type === SelectTrigger) {
            state.className = child.props.className;
            state.id = child.props.id;
        }
        collect(child.props.children, state);
    });
}
export function Select({ children, onValueChange, className, size, ...props }: RootProps) {
    const state: {
        options: NonNullable<SelectProps['options']>;
        placeholder?: ReactNode;
        className?: string;
        id?: string;
    } = { options: [] };
    collect(children, state);
    return (
        <AntSelect
            options={state.options}
            placeholder={state.placeholder}
            className={className ?? state.className}
            id={state.id}
            size={size === 'sm' ? 'small' : 'middle'}
            onChange={onValueChange}
            style={{ width: '100%' }}
            {...props}
        />
    );
}
export function SelectItem(_props: MarkerProps) {
    return null;
}
export function SelectValue(_props: MarkerProps) {
    return null;
}
export function SelectTrigger(_props: MarkerProps) {
    return null;
}
export function SelectContent({ children }: MarkerProps) {
    return <>{children}</>;
}
export function SelectGroup({ children }: MarkerProps) {
    return <>{children}</>;
}
export function SelectLabel({ children }: MarkerProps) {
    return <>{children}</>;
}
export function SelectSeparator() {
    return <Divider />;
}
export function SelectScrollUpButton() {
    return null;
}
export function SelectScrollDownButton() {
    return null;
}
