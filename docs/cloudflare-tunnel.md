# Cloudflare Tunnel Integration

## Goal

Expose the new solar dashboard at `solar.mydomain.com` without breaking the already-running website on the same server.

## Routing Principle

Do not replace the existing routing blindly.

The safe approach is:

1. keep the current website route exactly as it is
2. add a new hostname rule for the solar stack
3. point that new rule either to the new frontend container or to the existing reverse proxy, depending on the current setup

## Recommended Integration Patterns

### Pattern A: Tunnel Routes Directly to Containers

Use this when `cloudflared` is already on the same Docker network as the app services and is routing directly by hostname rule.

Example `config.yml` pattern:

```yaml
tunnel: <existing-tunnel-id>
credentials-file: /etc/cloudflared/<existing-tunnel-id>.json

ingress:
  - hostname: solar.mydomain.com
    service: http://frontend:3000
  - hostname: existing.mydomain.com
    service: http://existing-service:80
  - service: http_status:404
```

Important rule:

- keep the catch-all `http_status:404` last

### Pattern B: Tunnel Routes to an Existing Reverse Proxy

Use this when the current site already depends on `Nginx` or `Traefik`.

In this model:

- `cloudflared` keeps pointing to the reverse proxy
- the reverse proxy gets a new host-based rule for `solar.mydomain.com`
- the reverse proxy forwards that host to the new frontend service

This is often the lowest-risk option because it preserves the existing tunnel layout.

## Nginx Example

```nginx
server {
    listen 80;
    server_name solar.mydomain.com;

    location / {
        proxy_pass http://frontend:3000;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }
}
```

## Traefik Example

Key idea:

- create a router rule for `Host(\`solar.mydomain.com\`)`
- attach it only to the solar frontend service
- do not change the existing router for the old site

## Validation Checklist

After updating the tunnel or proxy rules:

1. verify the existing public site still resolves and serves normally
2. verify `solar.mydomain.com` resolves to the new frontend
3. verify live API traffic works through the chosen proxy path
4. verify there is no hostname collision with the old site
5. verify container restarts do not change routing assumptions

## Operational Notes

- Prefer host-based routing over path-based routing for clean isolation.
- Keep the solar stack on its own Docker network unless shared proxy access requires a controlled bridge.
- Document the final chosen routing pattern once the real server layout is inspected.

## Systemd Management

For reboot-safe operation on the primary server, run `cloudflared` under `systemd` instead of a manual `nohup` process.

Repository template:

- `server/cloudflared/cloudflared.service`
- `server/cloudflared/solar-tunnel.service`

Expected runtime details on the server:

- binary: `/usr/local/bin/cloudflared`
- config: `/home/server/.cloudflared/config.yml`
- credentials file: referenced from that `config.yml`

Recommended cutover:

1. install the `systemd` unit
2. run `systemctl daemon-reload`
3. `enable --now` the service
4. verify the new managed process is healthy
5. stop the old manually started process only after the managed service is up

Verification commands:

```bash
systemctl status cloudflared --no-pager
systemctl is-enabled cloudflared
journalctl -u cloudflared -f
```

### Named Solar Tunnel Service

If you want a dedicated unit name instead of `cloudflared.service`, use the repository template:

- `server/cloudflared/solar-tunnel.service`

Installation commands:

```bash
sudo install -m 644 /home/server/sollar_panel/server/cloudflared/solar-tunnel.service /etc/systemd/system/solar-tunnel.service
sudo systemctl daemon-reload
sudo systemctl enable --now solar-tunnel
sudo systemctl status solar-tunnel --no-pager
```
