import { Spin } from 'antd';
import type { ComponentProps } from 'react';
export function Spinner(props: ComponentProps<typeof Spin>) {
    return <Spin {...props} />;
}
export function LoadingScreen() {
    return (
        <div className="flex min-h-dvh items-center justify-center">
            <Spin size="large" tip="加载中…">
                <div className="h-20 w-40" />
            </Spin>
        </div>
    );
}
