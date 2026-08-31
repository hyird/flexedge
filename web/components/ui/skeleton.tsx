import { Skeleton as AntSkeleton } from 'antd';
import type { HTMLAttributes } from 'react';
export function Skeleton({ className, ...props }: HTMLAttributes<HTMLDivElement>) {
    return <AntSkeleton.Input active block className={className} {...props} />;
}
