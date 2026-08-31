import { Checkbox as AntCheckbox } from 'antd';
import type { CheckboxProps } from 'antd';
export function Checkbox({
    checked,
    onCheckedChange,
    ...props
}: CheckboxProps & { onCheckedChange?: (checked: boolean) => void }) {
    return (
        <AntCheckbox
            checked={checked}
            onChange={(event) => onCheckedChange?.(event.target.checked)}
            {...props}
        />
    );
}
