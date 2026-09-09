# Telegram upstream release notifications

The optional watcher checks official Telegram Desktop stable releases and sends
new versions to an authenticated ntfy topic. It runs separately from Telegravim
and does not clone repositories, run an AI agent, merge code, build an app,
install updates, or publish a release.

## Public code and private configuration

This repository contains reusable scripts and deployment templates. Examples
use `notify.example.com`; no installation is configured by default. Supply your
own domain explicitly when preparing a server.

Keep real domains, SSH aliases, device usernames, tokens, runtime configuration,
and deployment or device-test reports outside version control. Local operator
notes can live in the primary checkout's ignored `.local/private/ntfy/`
directory with mode `0700`; private files should use mode `0600`. An ignored
directory is not an encrypted backup. Store runtime credentials on the server
and keep encrypted backups separately.

PR descriptions, comments, logs, screenshots, Wiki pages, and old commits are
also public. Use placeholders there. Removing a file from the current tree does
not remove earlier published versions.

## New server installation

The templates require an SSH account, Docker Compose, Python 3, and cron.
Choose an HTTPS hostname that points to the server and can use TCP port 443.
Replace example addresses only in your private deployment configuration.

Copy `compose.yaml`, `Caddyfile`, and `provision.py` from `ops/ntfy/` to
`~/services/ntfy/`. Copy `ops/upstream-watch/upstream_watch.py` to
`~/.local/lib/telegravim-upstream-watch/`, and its cron template to
`~/services/ntfy/telegravim-upstream-watch.cron`.

```sh
python3 ~/services/ntfy/provision.py prepare --domain notify.example.com
docker compose --project-directory ~/services/ntfy config --quiet
docker compose --project-directory ~/services/ntfy run --rm --no-deps \
  caddy caddy validate --config /etc/caddy/Caddyfile
docker compose --project-directory ~/services/ntfy up -d --wait --wait-timeout 90
python3 ~/services/ntfy/provision.py connect-watcher
```

The provisioner generates private credentials using a pinned ntfy image with
network access disabled. Existing server configuration and credentials are
preserved. Recovery from an existing bootstrap preserves its device reader;
ambiguous reader permissions require manual review.

| Account in a new installation | Access |
| --- | --- |
| `subscriber` | Read the `telegravim-updates` topic; use on devices. |
| `telegravim-publisher` | Write that topic; used by the watcher. |
| `server-admin` | Administrative recovery; keep credentials on the server. |
| Anonymous | Denied; signup is disabled. |

Both containers run as the operator's nonroot UID with read-only root
filesystems, resource limits, and persistent data. Caddy publishes TCP 443;
ntfy has no published port. Certificate issuance uses TLS-ALPN-01 on port 443.
Outbound access is needed for certificate renewal and push delivery.

Generated files belong only in the private deployment:

```text
~/services/ntfy/.env
~/services/ntfy/config/server.yml
~/services/ntfy/secrets/
~/services/ntfy/data/
~/services/ntfy/backups/
~/.config/telegravim-upstream-watch/{config.json,ntfy.token}
~/.local/state/telegravim-upstream-watch/{state.json,state.json.lock}
```

Configuration reapplies users, access rules, and tokens when ntfy starts.
Credential changes must update private configuration and recovery material
consistently. Preserve Web Push keys during ordinary upgrades.

## Watcher configuration and scheduling

Copy `ops/upstream-watch/config.example.json` to a private configuration path.
All five keys are required:

| Key | Meaning |
| --- | --- |
| `baseline_version` | Initial version floor, `X.Y.Z` or `vX.Y.Z`. |
| `ntfy_url` | HTTPS base URL without credentials, topic, query, or fragment. |
| `ntfy_topic` | Topic with 1–64 letters, digits, underscores, or hyphens. |
| `ntfy_token_file` | Absolute or `~/` path to a private, user-owned token file. |
| `state_file` | Persistent delivery state, separate from configuration and token. |

The watcher refuses redirects. It sends the bearer token in the Authorization
header over verified HTTPS, never in URLs or cron entries. The token file must
be a regular, non-symlink file with private permissions. Unknown keys and invalid
versions fail validation.

Before enabling the schedule, run:

```sh
python3 ~/.local/lib/telegravim-upstream-watch/upstream_watch.py \
  --config ~/.config/telegravim-upstream-watch/config.json --dry-run
python3 ~/.local/lib/telegravim-upstream-watch/upstream_watch.py \
  --config ~/.config/telegravim-upstream-watch/config.json --test-notification
python3 ~/.local/lib/telegravim-upstream-watch/upstream_watch.py \
  --config ~/.config/telegravim-upstream-watch/config.json
```

Confirm authenticated receipt, then repeat the normal check: expect `no_update`.
Run `python3 ~/services/ntfy/provision.py schedule` only after a real release
notification succeeds. The installer preserves other cron jobs, backs up and
validates the crontab, and rejects conflicting watcher entries.

The template checks every six hours in the server's configured timezone. Runs
use a time limit and file lock. State advances atomically only after a valid
ntfy acknowledgement; failed deliveries remain eligible for a later retry.
`--dry-run` sends nothing and does not read tokens or change state.

## Device subscriptions

Use a client that supports authenticated subscriptions to a self-hosted ntfy
server. Configure the server URL, topic, and read-only account privately.
Do not use the administrator account or publisher token on a device.

Native iOS push may require the [ntfy wake-up relay](https://docs.ntfy.sh/config/#ios-instant-notifications).
The template enables that relay. It forwards a message identifier and a hash
of the topic URL; the client fetches message content from the configured server.
Background delivery depends on the selected client's push integration.

For browser notifications, follow the [ntfy web app guide](https://docs.ntfy.sh/subscribe/pwa/).
Allow notifications in the client and operating system. Test with the device
locked or the app in the background: a successful server response or manual
history refresh does not establish background delivery.

For another project, provision a separate topic and write-only publisher.
Do not reuse tokens across topics or grant wildcard access.

## Operation and verification

Before changes, privately back up server configuration, credentials, databases,
Web Push keys, and watcher state. A consistent SQLite backup requires stopping
only this service or using a supported database backup method. Keep an encrypted
off-host copy. Restore matching configuration, credentials, and data together.

Pause checks by removing only the watcher entry with `crontab -e`; preserve other
jobs and delivery state. Service removal must preserve bind-mounted private
data. Do not globally prune Docker as part of an update.

Run the isolated tests from the repository root:

```sh
python3 -m unittest discover -s ops/upstream-watch -p 'test_*.py'
python3 -m unittest discover -s ops/ntfy -p 'test_*.py'
```

For an actual deployment, also verify HTTPS, anonymous and cross-topic denial,
reader/publisher permissions, repeat-run deduplication, persistence across a
restart, and background delivery on each device. Keep installation-specific
results private. This guide does not certify any particular deployment.
