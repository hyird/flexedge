import { Select } from 'antd';
import type { SelectProps } from 'antd';
export interface ComboboxOption {
    value: string;
    label: string;
    description?: string;
    disabled?: boolean;
}
interface ComboboxProps {
    value?: string;
    options: ComboboxOption[];
    onValueChange: (value: string) => void;
    onSearchChange?: (value: string) => void;
    placeholder?: string;
    searchPlaceholder?: string;
    emptyText?: string;
    loading?: boolean;
    disabled?: boolean;
    invalid?: boolean;
    className?: string;
    id?: string;
    name?: string;
    onBlur?: () => void;
    'aria-describedby'?: string;
    'aria-labelledby'?: string;
}
export function Combobox({
    options,
    onValueChange,
    onSearchChange,
    emptyText = '没有匹配项',
    invalid,
    ...props
}: ComboboxProps) {
    const antOptions: SelectProps['options'] = options.map((item) => ({
        value: item.value,
        label: item.description ? (
            <div>
                <div>{item.label}</div>
                <div className="text-xs text-gray-400">{item.description}</div>
            </div>
        ) : (
            item.label
        ),
        disabled: item.disabled,
    }));
    return (
        <Select
            showSearch
            allowClear
            optionFilterProp="label"
            options={antOptions}
            onChange={onValueChange}
            onSearch={onSearchChange}
            notFoundContent={emptyText}
            status={invalid ? 'error' : undefined}
            style={{ width: '100%' }}
            {...props}
        />
    );
}
