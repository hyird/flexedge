export function createQueryKeys<T extends string>(module: T) {
    return {
        all: [module] as const,
        lists: () => [module, 'list'] as const,
        list: <P extends Record<string, unknown>>(params: P) => [module, 'list', params] as const,
        detail: (id: string) => [module, 'detail', id] as const,
    };
}
