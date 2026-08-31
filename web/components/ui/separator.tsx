import { Divider } from 'antd';
import type { ComponentProps } from 'react';
export function Separator({
    orientation = 'horizontal',
    ...props
}: ComponentProps<typeof Divider> & { orientation?: 'horizontal' | 'vertical' }) {
    return <Divider type={orientation === 'vertical' ? 'vertical' : 'horizontal'} {...props} />;
}
