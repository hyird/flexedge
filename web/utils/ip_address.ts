function isIpv4Address(value: string) {
    const parts = value.split('.');
    return (
        parts.length === 4 &&
        parts.every(
            (part) =>
                /^\d{1,3}$/.test(part) &&
                Number(part) >= 0 &&
                Number(part) <= 255 &&
                (part === '0' || !part.startsWith('0'))
        )
    );
}

function isIpv6Address(value: string) {
    if (!value.includes(':') || value.includes(':::') || value.split('::').length > 2) return false;

    let normalized = value;
    let ipv4Groups = 0;
    const lastColon = normalized.lastIndexOf(':');
    const tail = normalized.slice(lastColon + 1);
    if (tail.includes('.')) {
        if (!isIpv4Address(tail)) return false;
        normalized = `${normalized.slice(0, lastColon)}:ipv4`;
        ipv4Groups = 2;
    }

    const compressed = normalized.includes('::');
    const groups = normalized
        .split(':')
        .filter(Boolean)
        .filter((group) => group !== 'ipv4');
    if (!groups.every((group) => /^[0-9A-Fa-f]{1,4}$/.test(group))) return false;

    const groupCount = groups.length + ipv4Groups;
    return compressed ? groupCount < 8 : groupCount === 8;
}

export function isIpAddress(value: string) {
    return isIpv4Address(value) || isIpv6Address(value);
}

export { isIpv4Address };
