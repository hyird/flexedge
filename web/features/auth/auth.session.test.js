import { afterEach, describe, expect, test } from 'bun:test';
import { queryClient } from '@/config/query';
import { applyAuthenticatedSession, clearAuthenticatedSession } from '@/features/auth/auth.session';
import { useAuthStore } from '@/features/auth/auth.store';

function user(id = 'admin-1') {
    return {
        id,
        username: 'admin',
        status: 'enabled',
    };
}

afterEach(() => {
    queryClient.clear();
    useAuthStore.getState().clearAuth();
});

describe('authenticated query ownership', () => {
    test('changing identity removes business data but preserves the auth query', () => {
        queryClient.setQueryData(['auth', 'currentUser'], { marker: 'auth' });
        queryClient.setQueryData(['websites', 'list'], { marker: 'business' });

        applyAuthenticatedSession(user());

        expect(queryClient.getQueryData(['auth', 'currentUser'])).toEqual({ marker: 'auth' });
        expect(queryClient.getQueryData(['websites', 'list'])).toBeUndefined();
    });

    test('refreshing the same authorization context keeps business data', () => {
        applyAuthenticatedSession(user());
        queryClient.setQueryData(['websites', 'list'], { marker: 'business' });

        applyAuthenticatedSession(user());

        expect(queryClient.getQueryData(['websites', 'list'])).toEqual({ marker: 'business' });
    });

    test('administrator identity changes remove business data', () => {
        applyAuthenticatedSession(user('admin-1'));
        queryClient.setQueryData(['websites', 'list'], { marker: 'admin-1' });
        applyAuthenticatedSession(user('admin-2'));
        expect(queryClient.getQueryData(['websites', 'list'])).toBeUndefined();
    });

    test('ending the session clears every query and the identity', () => {
        applyAuthenticatedSession(user());
        queryClient.setQueryData(['websites', 'list'], { marker: 'business' });

        clearAuthenticatedSession();

        expect(queryClient.getQueryCache().getAll()).toHaveLength(0);
        expect(useAuthStore.getState().user).toBeNull();
    });
});
