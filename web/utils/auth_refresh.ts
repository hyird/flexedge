interface RefreshWaiter {
    resolve: () => void;
    reject: (error: unknown) => void;
}

export class AuthRefreshCoordinator {
    private active = false;
    private waiters: RefreshWaiter[] = [];

    tryStart() {
        if (this.active) return false;
        this.active = true;
        return true;
    }

    wait() {
        if (!this.active) throw new Error('cannot wait for an inactive token refresh');
        return new Promise<void>((resolve, reject) => {
            this.waiters.push({ resolve, reject });
        });
    }

    succeed() {
        const waiters = this.finish();
        waiters.forEach(({ resolve }) => {
            resolve();
        });
    }

    fail(error: unknown) {
        const waiters = this.finish();
        waiters.forEach(({ reject }) => {
            reject(error);
        });
    }

    private finish() {
        this.active = false;
        return this.waiters.splice(0);
    }
}
