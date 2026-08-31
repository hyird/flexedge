import { zodResolver } from '@hookform/resolvers/zod';
import { LoaderCircle, LockKeyhole, LogIn, ShieldCheck, UserRound } from 'lucide-react';
import { useEffect } from 'react';
import { useForm } from 'react-hook-form';
import { useLocation, useNavigate } from 'react-router-dom';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Field, FieldError, FieldGroup, FieldLabel } from '@/components/ui/field';
import { Input, PasswordInput } from '@/components/ui/input';
import { APP_NAME, getAppTitle } from '@/config/app';
import { useLogin } from '@/features/auth/auth.service';
import { useAuthStore } from '@/features/auth/auth.store';
import type { LoginRequest } from '@/features/auth/auth.types';
import { loginSchema, PASSWORD_MAX_LENGTH, USERNAME_MAX_LENGTH } from './auth.schema';

interface LocationState {
    from?: {
        pathname: string;
    };
}

const pageTitle = getAppTitle('登录系统');

export function LoginPage() {
    const navigate = useNavigate();
    const location = useLocation();
    const user = useAuthStore((state) => state.user);
    const mutation = useLogin();
    const {
        register,
        handleSubmit,
        formState: { errors },
    } = useForm<LoginRequest>({
        resolver: zodResolver(loginSchema),
        defaultValues: { username: '', password: '' },
    });

    useEffect(() => {
        if (user && !mutation.isPending) {
            const from = (location.state as LocationState)?.from?.pathname || '/overview';
            navigate(from, { replace: true });
        }
    }, [user, mutation.isPending, location.state, navigate]);

    useEffect(() => {
        const previousTitle = document.title;
        document.title = pageTitle;

        return () => {
            document.title = previousTitle;
        };
    }, []);

    const submit = (values: LoginRequest) => {
        if (!mutation.isPending) mutation.mutate(values);
    };

    return (
        <main className="relative flex min-h-dvh w-full items-center justify-center overflow-hidden bg-[radial-gradient(circle_at_top_left,color-mix(in_oklab,var(--primary)_18%,transparent),transparent_38%),linear-gradient(180deg,var(--background),color-mix(in_oklab,var(--muted)_62%,white))] p-4 sm:p-6">
            <div className="pointer-events-none absolute inset-x-0 top-0 h-px bg-gradient-to-r from-transparent via-primary/50 to-transparent" />
            <Card className="relative w-full max-w-md border-border/70 bg-card/95 shadow-2xl shadow-primary/10 backdrop-blur">
                <CardHeader className="pb-1">
                    <div className="mb-3 flex size-11 items-center justify-center rounded-xl bg-primary/10 text-primary ring-1 ring-primary/15">
                        <ShieldCheck className="size-6" />
                    </div>
                    <p className="text-xs font-semibold uppercase tracking-[0.16em] text-primary">
                        {APP_NAME}
                    </p>
                    <CardTitle className="text-2xl">登录系统</CardTitle>
                    <CardDescription>使用管理员账户进入边缘交付控制面</CardDescription>
                </CardHeader>
                <CardContent>
                    <form
                        className="mt-3"
                        autoComplete="on"
                        noValidate
                        onSubmit={handleSubmit(submit)}
                    >
                        <FieldGroup>
                            <Field data-invalid={Boolean(errors.username)}>
                                <FieldLabel htmlFor="username">用户名</FieldLabel>
                                <div className="relative">
                                    <UserRound className="pointer-events-none absolute left-3 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
                                    <Input
                                        id="username"
                                        autoComplete="username"
                                        maxLength={USERNAME_MAX_LENGTH}
                                        placeholder="请输入用户名"
                                        className="pl-9"
                                        aria-invalid={Boolean(errors.username)}
                                        {...register('username')}
                                    />
                                </div>
                                <FieldError>{errors.username?.message}</FieldError>
                            </Field>
                            <Field data-invalid={Boolean(errors.password)}>
                                <FieldLabel htmlFor="password">密码</FieldLabel>
                                <div className="relative">
                                    <LockKeyhole className="pointer-events-none absolute left-3 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
                                    <PasswordInput
                                        id="password"
                                        autoComplete="current-password"
                                        maxLength={PASSWORD_MAX_LENGTH}
                                        placeholder="请输入密码"
                                        className="px-9"
                                        aria-invalid={Boolean(errors.password)}
                                        {...register('password')}
                                    />
                                </div>
                                <FieldError>{errors.password?.message}</FieldError>
                            </Field>
                            <Button
                                type="submit"
                                size="lg"
                                className="mt-1 w-full"
                                disabled={mutation.isPending}
                            >
                                {mutation.isPending ? (
                                    <LoaderCircle className="animate-spin" />
                                ) : (
                                    <LogIn />
                                )}
                                {mutation.isPending ? '正在登录' : '登录系统'}
                            </Button>
                        </FieldGroup>
                    </form>
                </CardContent>
            </Card>
        </main>
    );
}

export default LoginPage;
