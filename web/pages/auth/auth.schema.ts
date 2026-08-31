import { z } from 'zod';

export const USERNAME_MAX_LENGTH = 50;
export const PASSWORD_MAX_LENGTH = 1024;

export const loginSchema = z.object({
    username: z.string().min(1, '用户名不能为空').max(USERNAME_MAX_LENGTH, '用户名最多50个字符'),
    password: z.string().min(1, '密码不能为空').max(PASSWORD_MAX_LENGTH, '密码最多1024个字符'),
});
