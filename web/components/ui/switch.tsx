import { Switch as AntSwitch } from 'antd';
import type { SwitchProps } from 'antd';
export function Switch({
    size,
    onCheckedChange,
    ...props
}: Omit<SwitchProps, 'size'> & {
    size?: SwitchProps['size'] | 'sm';
    onCheckedChange?: (checked: boolean) => void;
}) {
    return (
        <AntSwitch
            size={size === 'small' || size === 'sm' ? 'small' : 'default'}
            onChange={onCheckedChange}
            {...props}
        />
    );
}
