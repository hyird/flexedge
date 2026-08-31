import { QueryClient } from '@tanstack/react-query';

export const queryClient = new QueryClient({
    defaultOptions: {
        queries: {
            staleTime: 5 * 60 * 1000,
            gcTime: 10 * 60 * 1000,
            refetchOnWindowFocus: false,
            refetchOnReconnect: true,
            retry: 1,
            // Keep the previous result visible while a filter/page query is fetched.
            // This avoids empty-state/skeleton flashes during ordinary navigation.
            placeholderData: (previousData: unknown) => previousData,
        },
        mutations: {
            retry: 0,
        },
    },
});
