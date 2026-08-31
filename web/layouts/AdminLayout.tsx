import {
    CloudServerOutlined,
    DashboardOutlined,
    GlobalOutlined,
    KeyOutlined,
    LogoutOutlined,
    MenuFoldOutlined,
    MenuOutlined,
    MenuUnfoldOutlined,
    SafetyCertificateOutlined,
    SearchOutlined,
    UserOutlined,
} from '@ant-design/icons';
import {
    Breadcrumb,
    Button,
    Drawer,
    Dropdown,
    Grid,
    Input,
    Layout,
    Menu,
    Space,
    Typography,
} from 'antd';
import type { MenuProps } from 'antd';
import { useMemo, useState } from 'react';
import { useLocation, useNavigate, useOutlet } from 'react-router-dom';
import { APP_NAME } from '@/config/app';
import { useLogout } from '@/features/auth/auth.service';
import { useAuthStore } from '@/features/auth/auth.store';
import { useTaskCompletionRefresh } from '@/hooks/useTaskCompletionRefresh';
import TaskEntry from '@/pages/task';

const { Header, Sider, Content } = Layout;
const NAVIGATION_GROUPS = [
    {
        label: '工作台',
        items: [{ path: '/overview', label: '首页概览', icon: <DashboardOutlined /> }],
    },
    {
        label: '边缘交付',
        items: [
            { path: '/clusters', label: '边缘节点', icon: <CloudServerOutlined /> },
            { path: '/websites', label: '网站管理', icon: <GlobalOutlined /> },
        ],
    },
    {
        label: '域名服务',
        items: [
            { path: '/dns-providers', label: 'DNS 服务商', icon: <KeyOutlined /> },
            { path: '/dns-zones', label: '域名管理', icon: <GlobalOutlined /> },
        ],
    },
    {
        label: '安全服务',
        items: [{ path: '/certificates', label: '证书管理', icon: <SafetyCertificateOutlined /> }],
    },
];
const navigationItems = NAVIGATION_GROUPS.flatMap((group) => group.items);

export default function AdminLayout() {
    useTaskCompletionRefresh();
    const location = useLocation();
    const navigate = useNavigate();
    const outlet = useOutlet();
    const screens = Grid.useBreakpoint();
    const logout = useLogout();
    const user = useAuthStore((state) => state.user);
    const adminId = user?.id;
    const [collapsed, setCollapsed] = useState(false);
    const [mobileNavigationOpen, setMobileNavigationOpen] = useState(false);
    const [menuKeyword, setMenuKeyword] = useState('');
    const currentItem =
        navigationItems.find(
            (item) =>
                location.pathname === item.path || location.pathname.startsWith(`${item.path}/`)
        ) ?? navigationItems[0];
    const currentGroup =
        NAVIGATION_GROUPS.find((group) =>
            group.items.some((item) => item.path === currentItem.path)
        ) ?? NAVIGATION_GROUPS[0];
    const userLabel = user?.nickname?.trim() || user?.username || '管理员';
    const menuItems = useMemo<MenuProps['items']>(
        () =>
            NAVIGATION_GROUPS.map((group) => ({
                key: `group-${group.label}`,
                type: 'group' as const,
                label: group.label,
                children: group.items
                    .filter((item) => item.label.includes(menuKeyword.trim()))
                    .map((item) => ({ key: item.path, label: item.label, icon: item.icon })),
            })).filter((group) => (group.children?.length ?? 0) > 0),
        [menuKeyword]
    );
    const navigateTo = (path: string) => {
        setMobileNavigationOpen(false);
        navigate(path);
    };
    const navigation = (isCollapsed: boolean) => (
        <div className="flex h-full min-h-0 flex-col">
            <div className="flex h-14 shrink-0 items-center justify-center border-b border-white/10 px-3 text-white">
                <div className="flex size-8 items-center justify-center rounded-antd bg-primary font-bold">
                    FX
                </div>
                {!isCollapsed && (
                    <div className="ml-3 min-w-0">
                        <div className="truncate font-semibold">{APP_NAME}</div>
                        <div className="truncate text-[11px] text-white/45">边缘交付控制面</div>
                    </div>
                )}
            </div>
            {!isCollapsed && (
                <div className="px-3 py-3">
                    <Input
                        allowClear
                        prefix={<SearchOutlined />}
                        placeholder="搜索菜单"
                        value={menuKeyword}
                        onChange={(event) => setMenuKeyword(event.target.value)}
                        className="sider-search"
                    />
                </div>
            )}
            <Menu
                theme="dark"
                mode="inline"
                inlineCollapsed={isCollapsed}
                selectedKeys={[currentItem.path]}
                items={menuItems}
                onClick={({ key }) => navigateTo(key)}
                className="min-h-0 flex-1 overflow-y-auto border-e-0"
            />
        </div>
    );
    const userMenu: MenuProps = {
        items: [{ key: 'logout', icon: <LogoutOutlined />, label: '退出登录', danger: true }],
        onClick: ({ key }) => key === 'logout' && logout.mutate(),
    };

    return (
        <Layout className="h-dvh min-h-0 overflow-hidden">
            {screens.md && (
                <Sider
                    width={224}
                    collapsedWidth={64}
                    collapsed={collapsed}
                    trigger={null}
                    theme="dark"
                >
                    {navigation(collapsed)}
                </Sider>
            )}
            <Drawer
                open={mobileNavigationOpen}
                onClose={() => setMobileNavigationOpen(false)}
                placement="left"
                width={280}
                closable={false}
                styles={{ body: { padding: 0, background: 'var(--app-sidebar)' } }}
            >
                {navigation(false)}
            </Drawer>
            <Layout className="min-w-0 bg-background">
                <Header className="flex h-14 shrink-0 items-center gap-3 bg-white px-4 shadow-[0_1px_4px_rgba(0,0,0,0.08)]">
                    <Button
                        type="text"
                        icon={
                            screens.md ? (
                                collapsed ? (
                                    <MenuUnfoldOutlined />
                                ) : (
                                    <MenuFoldOutlined />
                                )
                            ) : (
                                <MenuOutlined />
                            )
                        }
                        onClick={() =>
                            screens.md
                                ? setCollapsed((value) => !value)
                                : setMobileNavigationOpen(true)
                        }
                    />
                    <Breadcrumb
                        items={[{ title: currentGroup.label }, { title: currentItem.label }]}
                    />
                    <div className="flex-1" />
                    <TaskEntry />
                    <Dropdown menu={userMenu} placement="bottomRight" trigger={['click']}>
                        <Button>
                            <Space>
                                <UserOutlined />
                                <Typography.Text className="max-w-32 truncate">
                                    {userLabel}
                                </Typography.Text>
                            </Space>
                        </Button>
                    </Dropdown>
                </Header>
                <Content className="m-4 min-h-0 flex-1 overflow-hidden rounded-lg bg-white shadow-sm">
                    <main className="min-h-0 h-full flex-1 overflow-hidden">
                        <div key={adminId} className="h-full min-h-0 min-w-0 overflow-hidden">
                            {outlet}
                        </div>
                    </main>
                </Content>
            </Layout>
        </Layout>
    );
}
