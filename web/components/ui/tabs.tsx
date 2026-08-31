import { Tabs as AntTabs } from 'antd';
import { Children, isValidElement, type ReactElement, type ReactNode } from 'react';
import { cn } from '@/lib/utils';
type MarkerProps = {
    value?: string;
    children?: ReactNode;
    className?: string;
    disabled?: boolean;
    variant?: 'default' | 'line';
};
type TabsProps = MarkerProps & {
    defaultValue?: string;
    onValueChange?: (value: string) => void;
    orientation?: 'horizontal' | 'vertical';
};
function find(children: ReactNode, type: unknown, result: ReactElement<MarkerProps>[] = []) {
    Children.forEach(children, (child) => {
        if (!isValidElement<MarkerProps>(child)) return;
        if (child.type === type) result.push(child);
        find(child.props.children, type, result);
    });
    return result;
}
export function Tabs({
    children,
    value,
    defaultValue,
    onValueChange,
    orientation,
    className,
}: TabsProps) {
    const triggers = find(children, TabsTrigger);
    const contents = find(children, TabsContent);
    const items = triggers.map((trigger) => ({
        key: trigger.props.value ?? '',
        label: trigger.props.children,
        disabled: trigger.props.disabled,
        children: (() => {
            const content = contents.find((entry) => entry.props.value === trigger.props.value);
            return content ? (
                <div className={cn('min-h-0', content.props.className)}>
                    {content.props.children}
                </div>
            ) : null;
        })(),
    }));
    return (
        <AntTabs
            activeKey={value}
            defaultActiveKey={defaultValue}
            onChange={onValueChange}
            tabPosition={orientation === 'vertical' ? 'left' : 'top'}
            items={items}
            className={cn('ant-business-tabs', className)}
        />
    );
}
export function TabsList({ children }: MarkerProps) {
    return <>{children}</>;
}
export function TabsTrigger(_props: MarkerProps) {
    return null;
}
export function TabsContent(_props: MarkerProps) {
    return null;
}
export const tabsListVariants = () => '';
