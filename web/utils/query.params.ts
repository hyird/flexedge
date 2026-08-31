function toWireKey(key: string): string {
    return key.replace(/[A-Z]/g, (letter) => `_${letter.toLowerCase()}`);
}

export function appendQueryParams<T extends object>(url: string, params?: T) {
    if (!params) {
        return url;
    }

    const searchParams = new URLSearchParams();

    Object.entries(params as Record<string, unknown>).forEach(([key, value]) => {
        if (value === undefined || value === null || value === '') {
            return;
        }
        searchParams.set(toWireKey(key), String(value));
    });

    const queryString = searchParams.toString();
    if (!queryString) return url;
    const separator = url.includes('?') ? (url.endsWith('?') || url.endsWith('&') ? '' : '&') : '?';
    return `${url}${separator}${queryString}`;
}
