import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { describe, expect, test } from 'bun:test';

const apiContracts = [
    {
        path: 'certificate/certificate.api.ts',
        allowedReads: [],
    },
    { path: 'cluster/cluster.api.ts', allowedReads: [] },
    { path: 'dns_zone/dns_zone.api.ts', allowedReads: [] },
    { path: 'cluster/node/node.api.ts', allowedReads: [] },
    { path: 'provider/certificate_provider.api.ts', allowedReads: [] },
    { path: 'provider/dns_provider.api.ts', allowedReads: [] },
    { path: 'task/task.api.ts', allowedReads: [] },
    { path: 'website/website.api.ts', allowedReads: [] },
];

describe('revision contract', () => {
    for (const contract of apiContracts) {
        test(`${contract.path} has only reviewed preflight reads`, () => {
            const path = join(resolve(import.meta.dir, '..', 'pages'), contract.path);
            const source = readFileSync(path, 'utf8');
            const reads = [...source.matchAll(/\bawait\s+(get[A-Z]\w*)\s*\(/g)].map(
                (match) => match[1]
            );
            expect(reads).toEqual(contract.allowedReads);
        });
    }
});
