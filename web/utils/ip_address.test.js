import { describe, expect, test } from 'bun:test';
import { isIpAddress, isIpv4Address } from './ip_address';

describe('IP address validation', () => {
    test('accepts canonical IPv4 and IPv6 addresses', () => {
        expect(isIpv4Address('192.0.2.1')).toBe(true);
        expect(isIpAddress('2001:db8::1')).toBe(true);
    });

    test('rejects invalid and ambiguous IPv4 spellings', () => {
        expect(isIpv4Address('999.0.0.1')).toBe(false);
        expect(isIpv4Address('192.168.001.1')).toBe(false);
        expect(isIpAddress('192.168.1')).toBe(false);
    });
});
