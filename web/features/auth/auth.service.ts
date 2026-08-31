/**
 * 跨页面认证查询与操作
 */

import { useMutation, useQuery } from '@tanstack/react-query';
import { useNavigate } from 'react-router-dom';
import { toast } from '@/components/ui/notification';
import { fetchCurrentUser, login, logout } from './auth.api';
import {
    applyAuthenticatedSession,
    applyAuthenticatedUser,
    clearAuthenticatedSession,
} from './auth.session';
import { useAuthStore } from './auth.store';
import type { AuthSessionResult, UserInfo } from './auth.types';

function sameUser(left: UserInfo | null, right: UserInfo) {
    return (
        left?.id === right.id &&
        left.username === right.username &&
        left.nickname === right.nickname &&
        left.status === right.status
    );
}

export const loginKeys = {
    currentUser: ['auth', 'currentUser'] as const,
};

export function useCurrentUser() {
    const user = useAuthStore((s) => s.user);

    return useQuery({
        queryKey: loginKeys.currentUser,
        queryFn: async () => {
            const freshUser = await fetchCurrentUser();
            if (!sameUser(user, freshUser)) {
                applyAuthenticatedUser(freshUser);
            }
            return freshUser;
        },
        enabled: !!user,
        initialData: user ?? undefined,
        initialDataUpdatedAt: 0,
        staleTime: 2 * 60 * 1000,
        refetchInterval: 5 * 60 * 1000,
        refetchOnWindowFocus: true,
    });
}

export function useLogin() {
    return useMutation({
        mutationFn: login,
        onSuccess: (data: AuthSessionResult) => {
            applyAuthenticatedSession(data.user);
            toast.success('登录成功');
        },
    });
}

export function useLogout() {
    const navigate = useNavigate();
    return useMutation({
        mutationFn: () => logout(),
        onSettled: () => {
            clearAuthenticatedSession();
            navigate('/login', { replace: true });
        },
    });
}
