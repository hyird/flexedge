import { LockOutlined, SafetyCertificateOutlined, UserOutlined } from '@ant-design/icons';
import { Button, Card, Form, Input, Typography } from 'antd';
import { useEffect } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import { APP_NAME, getAppTitle } from '@/config/app';
import { useLogin } from '@/features/auth/auth.service';
import { useAuthStore } from '@/features/auth/auth.store';
import type { LoginRequest } from '@/features/auth/auth.types';
import { PASSWORD_MAX_LENGTH, USERNAME_MAX_LENGTH } from './auth.schema';

interface LocationState {
    from?: {
        pathname: string;
    };
}

const pageTitle = getAppTitle('登录系统');
const { Text, Title } = Typography;

export function LoginPage() {
    const [form] = Form.useForm<LoginRequest>();
    const navigate = useNavigate();
    const location = useLocation();
    const user = useAuthStore((state) => state.user);
    const mutation = useLogin();

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
        <main className="relative flex min-h-dvh w-full items-center justify-center overflow-hidden bg-[linear-gradient(180deg,#fbfdff_0%,#eef4ff_48%,#f8fbff_100%)] p-6 sm:p-12">
            <div
                aria-hidden="true"
                className="pointer-events-none absolute -top-44 -left-44 size-[520px] rounded-full bg-[radial-gradient(circle,rgba(59,130,246,0.18),rgba(59,130,246,0.04)_64%,transparent_76%)] opacity-75 blur-[30px]"
            />
            <div
                aria-hidden="true"
                className="pointer-events-none absolute -right-56 -bottom-60 size-[620px] rounded-full bg-[radial-gradient(circle,rgba(37,99,235,0.12),rgba(37,99,235,0.03)_66%,transparent_78%)] opacity-75 blur-[30px]"
            />
            <div
                aria-hidden="true"
                className="pointer-events-none absolute top-[10%] right-[12%] size-[min(38vw,460px)] min-h-[300px] min-w-[300px] rounded-full bg-[radial-gradient(circle,rgba(255,255,255,0.82)_0%,rgba(255,255,255,0.26)_40%,transparent_72%)] opacity-85 blur-[14px]"
            />
            <div
                aria-hidden="true"
                className="pointer-events-none absolute inset-0 bg-[linear-gradient(135deg,transparent_0%,rgba(255,255,255,0.16)_42%,rgba(255,255,255,0.3)_52%,transparent_68%)] opacity-70 [mask-image:radial-gradient(circle_at_center,black_0%,transparent_76%)]"
            />

            <div className="relative z-10 w-full max-w-[440px]">
                <Card
                    className="border-white/70 bg-white/95 shadow-[0_18px_44px_rgba(15,23,42,0.08)] backdrop-blur-xl"
                    styles={{ body: { padding: 'clamp(28px, 4vw, 36px)' } }}
                >
                    <div className="mb-5 flex items-center gap-3">
                        <div className="flex size-11 items-center justify-center rounded-antd border border-blue-200/50 bg-blue-50 text-primary">
                            <SafetyCertificateOutlined className="text-xl" />
                        </div>
                        <div>
                            <Text className="text-xs tracking-[0.08em] text-gray-500 uppercase">
                                {APP_NAME}
                            </Text>
                            <Title level={3} style={{ margin: 0 }}>
                                登录系统
                            </Title>
                        </div>
                    </div>

                    <Text type="secondary">使用管理员账户进入边缘交付控制面</Text>

                    <Form<LoginRequest>
                        form={form}
                        layout="vertical"
                        className="mt-6"
                        autoComplete="on"
                        onFinish={submit}
                    >
                        <Form.Item
                            label="用户名"
                            name="username"
                            rules={[
                                { required: true, message: '用户名不能为空' },
                                {
                                    max: USERNAME_MAX_LENGTH,
                                    message: `用户名最多${USERNAME_MAX_LENGTH}个字符`,
                                },
                            ]}
                        >
                            <Input
                                autoComplete="username"
                                maxLength={USERNAME_MAX_LENGTH}
                                placeholder="请输入用户名"
                                prefix={<UserOutlined />}
                            />
                        </Form.Item>
                        <Form.Item
                            label="密码"
                            name="password"
                            rules={[
                                { required: true, message: '密码不能为空' },
                                {
                                    max: PASSWORD_MAX_LENGTH,
                                    message: `密码最多${PASSWORD_MAX_LENGTH}个字符`,
                                },
                            ]}
                        >
                            <Input.Password
                                autoComplete="current-password"
                                maxLength={PASSWORD_MAX_LENGTH}
                                placeholder="请输入密码"
                                prefix={<LockOutlined />}
                            />
                        </Form.Item>
                        <Form.Item className="mb-0 mt-2">
                            <Button
                                type="primary"
                                htmlType="submit"
                                block
                                size="large"
                                disabled={mutation.isPending}
                                loading={mutation.isPending}
                            >
                                登录系统
                            </Button>
                        </Form.Item>
                    </Form>
                </Card>
            </div>
        </main>
    );
}

export default LoginPage;
