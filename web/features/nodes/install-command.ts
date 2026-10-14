import type { NodeCredentials } from './data'

const quote = (value: string) => `'${value.replaceAll("'", `'\\''`)}'`

/** One-line installer for a Linux node: downloads the control plane's
 * `install-node.sh` (`GET /api/agent/install-node.sh`) and runs it as root with
 * `FLEXEDGE_SERVER_ORIGIN` and the positional `<node-id> <secret>` it expects. */
export function nodeInstallCommand(
  origin: string,
  credentials: Pick<NodeCredentials, 'node_id' | 'secret'>
) {
  const server = origin.replace(/\/+$/, '')
  return [
    `curl -fsSL ${quote(`${server}/api/agent/install-node.sh`)} -o install-node.sh`,
    `sudo FLEXEDGE_SERVER_ORIGIN=${quote(server)} bash install-node.sh ${quote(credentials.node_id)} ${quote(credentials.secret)}`,
  ].join(' && \\\n  ')
}
