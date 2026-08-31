import { Input } from 'antd';
import type { TextAreaProps } from 'antd/es/input';
import { forwardRef } from 'react';
export const Textarea = forwardRef<unknown, TextAreaProps>((props, ref) => (
    <Input.TextArea
        ref={ref as never}
        status={props['aria-invalid'] ? 'error' : undefined}
        {...props}
    />
));
Textarea.displayName = 'Textarea';
