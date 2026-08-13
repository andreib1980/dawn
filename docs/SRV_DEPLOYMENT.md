# SRV Deployment

This fork deploys DAWN to the dedicated Ubuntu server through GitHub Actions
after the normal `CI` workflow validates a push to `main`.

## Topology

```text
Internal DNS
jarvis.abpfa.network -> 10.0.254.230
                               |
                         dawn-ingress
                    Caddy, HTTPS, DNS-01
                               |
                         dawn_backend
                               |
                            dawn:3000
                               |
                 Ollama at 10.0.248.128:11434
```

DAWN does not publish port 3000 on the host or infrastructure VLAN. Only the
Caddy ingress receives an infrastructure address.

The initial production endpoint is `https://jarvis.abpfa.network`.
`jarvis.abpfa.com` is intentionally outside this deployment until external
routing and access policy are separately approved.

## Runtime Resources

The Compose project uses:

- container `dawn`;
- container `dawn-ingress`;
- private bridge network `dawn_backend`;
- external IPvlan network `dawn_infra`;
- external volume `dawn_data`;
- external volumes `dawn_caddy_data` and `dawn_caddy_config`.

The ingress owns `10.0.254.230` on `dawn_infra`.

The expected IPvlan contract is:

- driver: `ipvlan`;
- mode: `l2`;
- parent: `enp2s0.254`;
- subnet: `10.0.254.0/24`;
- allocation range: `10.0.254.224/27`;
- gateway: `10.0.254.1`.

The SRV host reaches infrastructure containers through its existing
`infra-host` interface.

## Persistent Configuration

The deployment expects these host paths:

```text
/home/andrei/services/dawn/config/dawn.toml
/home/andrei/services/dawn/config/secrets.toml
/home/andrei/services/dawn/config/deploy.env
/home/andrei/services/dawn/models/whisper.cpp/ggml-base.en.bin
```

`deploy.env` must have mode `0600` and contain:

```text
DAWN_CLOUDFLARE_DNS_API_TOKEN=<scoped-token>
```

The Cloudflare token must be restricted to DNS record editing for the relevant
zone. It must never be committed.

DAWN identity, statistics, memory, and other runtime state persist in
`dawn_data`. Caddy certificate state persists independently in its two named
volumes.

## CI/CD

`.github/workflows/ci.yml` remains the validation authority. It checks
formatting, unit tests, the DAWN image, the Caddy ingress image, satellites, and
the SRV Compose contract.

`.github/workflows/deploy-srv.yml` runs only after a successful trusted `push`
workflow on `main`. The deployment job uses the dedicated `dawn-srv`
self-hosted runner and verifies that the checked-out commit is exactly the
commit validated by CI.

The deploy script:

1. validates the commit and clean checkout;
2. validates configuration, models, volumes, IPvlan, and Ollama;
3. builds commit-addressed DAWN and ingress images;
4. records the currently deployed images;
5. stops `dawn-poc` only immediately before cutover;
6. starts the managed Compose project;
7. verifies both container health checks;
8. verifies ingress plaintext health and trusted HTTPS;
9. verifies persisted DAWN databases and the configured Ollama model.

On failure, it restores the previous managed image pair. During the first
managed deployment, it removes the failed Compose project and restarts
`dawn-poc`.

## Required Repository Secrets

The DAWN GitHub repository requires:

- `DAWN_TELEGRAM_BOT_TOKEN`;
- `DAWN_TELEGRAM_CHAT_ID`.

They are used only for deployment-result notifications. The Cloudflare token is
stored only on SRV because certificate issuance occurs there.

## Initial Cutover

Do not manually stop or remove `dawn-poc`. The deployment script owns the
cutover and rollback.

After the first successful managed deployment, verify:

```text
docker inspect dawn
docker inspect dawn-ingress
https://jarvis.abpfa.network
existing DAWN administrator login
Ollama-backed response
Telegram deployment notification
```

Retain the POC container and its old volumes until the managed endpoint,
administrator identity, and persisted data have all been confirmed. Cleanup is
a separate, explicit operation.
