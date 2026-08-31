import type { HttpAuthSession } from '@/utils/http';
import { queryClient } from '@/config/query';
import { refreshSession as refreshSessionRequest } from './auth.api';
import { useAuthStore } from './auth.store';
import type { UserInfo } from './auth.types';

let initialization: Promise<void> | undefined;

function sameAuthorizationContext(current: UserInfo | null, next: UserInfo) {
    return current?.id === next.id;
}

function clearBusinessQueries() {
    queryClient.removeQueries({
        predicate: (query) => query.queryKey[0] !== 'auth',
    });
}

export function applyAuthenticatedSession(user: UserInfo) {
    const current = useAuthStore.getState().user;
    if (!sameAuthorizationContext(current, user)) clearBusinessQueries();
    useAuthStore.getState().setUser(user);
}

export function applyAuthenticatedUser(user: UserInfo) {
    const current = useAuthStore.getState().user;
    if (!sameAuthorizationContext(current, user)) clearBusinessQueries();
    useAuthStore.getState().setUser(user);
}

export function clearAuthenticatedSession() {
    queryClient.clear();
    useAuthStore.getState().clearAuth();
}

async function refreshAuthenticatedSession(): Promise<boolean> {
    try {
        const { user } = await refreshSessionRequest({ _silent: true });
        applyAuthenticatedSession(user);
        return true;
    } catch {
        clearAuthenticatedSession();
        return false;
    }
}

export function initializeAuthSession() {
    initialization ??= refreshAuthenticatedSession().then(() => undefined);
    return initialization;
}

export const httpAuthSession: HttpAuthSession = Object.freeze({
    refresh: refreshAuthenticatedSession,
    clear: clearAuthenticatedSession,
});
