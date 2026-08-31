import { useEffect } from 'react';
import { initializeAuthSession } from '@/features/auth/auth.session';
import { useAuthStore } from '@/features/auth/auth.store';

/**
 * 初始化用户认证数据
 * 使用 TanStack Query 管理用户数据获取和刷新
 */
export function useInitAuth() {
    const user = useAuthStore((s) => s.user);
    const initialized = useAuthStore((s) => s.initialized);

    useEffect(() => {
        void initializeAuthSession();
    }, []);

    return { user, initialized };
}
