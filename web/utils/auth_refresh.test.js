import { describe, expect, test } from 'bun:test';
import { AuthRefreshCoordinator } from './auth_refresh';

describe('AuthRefreshCoordinator', () => {
    test('allows only one refresh owner and resolves all waiters', async () => {
        const coordinator = new AuthRefreshCoordinator();
        expect(coordinator.tryStart()).toBe(true);
        expect(coordinator.tryStart()).toBe(false);

        const first = coordinator.wait();
        const second = coordinator.wait();
        coordinator.succeed();

        await expect(first).resolves.toBeUndefined();
        await expect(second).resolves.toBeUndefined();
        expect(coordinator.tryStart()).toBe(true);
    });

    test('rejects all waiters and permits a later refresh after failure', async () => {
        const coordinator = new AuthRefreshCoordinator();
        const failure = new Error('refresh failed');
        expect(coordinator.tryStart()).toBe(true);

        const waiting = coordinator.wait();
        coordinator.fail(failure);

        await expect(waiting).rejects.toBe(failure);
        expect(coordinator.tryStart()).toBe(true);
    });

    test('does not allow waiting without an active refresh', () => {
        const coordinator = new AuthRefreshCoordinator();
        expect(() => coordinator.wait()).toThrow('cannot wait for an inactive token refresh');
    });
});
