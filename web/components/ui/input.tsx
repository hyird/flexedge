import { Input as AntInput } from 'antd';
import type { InputProps } from 'antd';
import { forwardRef } from 'react';
export const Input = forwardRef<unknown, InputProps>((props, ref) => (
    <AntInput ref={ref as never} status={props['aria-invalid'] ? 'error' : undefined} {...props} />
));
Input.displayName = 'Input';

export const PasswordInput = forwardRef<unknown, InputProps>((props, ref) => (
    <AntInput.Password
        ref={ref as never}
        status={props['aria-invalid'] ? 'error' : undefined}
        {...props}
    />
));
PasswordInput.displayName = 'PasswordInput';
