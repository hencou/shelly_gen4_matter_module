#!/usr/bin/env bash
#
# shelly-overview.sh — generate a clickable HTML overview of every Shelly Gen4
# Matter module that advertises itself over mDNS (_http._tcp) on the LAN.
#
# Each module publishes an "_http._tcp" service (see matter_srp_advertise_httpd()
# in main/matter_device.cpp) whose instance label is the configured hostname and
# whose SRV/AAAA record points at the module's Thread OMR IPv6 address on port 80.
# A Thread border router with an advertising proxy re-publishes these as LAN mDNS,
# so a normal mDNS browser (avahi/dns-sd) can discover and open them.
#
# Usage:
#   ./shelly-overview.sh              # scan and open shelly-overview.html
#   ./shelly-overview.sh --all        # show ALL _http._tcp, not just Matter modules
#   ./shelly-overview.sh --name PFX   # only instance names starting with PFX
#   ./shelly-overview.sh --no-open    # only generate, do not open
#
# By default only Shelly Gen4 Matter modules are shown. They are recognised by
# their CHIP SRP hostname — an opaque 16-hex-character ".local" name (e.g.
# 6E4C8E3E56A7E8EE.local) — so the friendly instance name (Kantoor, ...) does
# not matter and normal LAN web services (printers, receivers, ...) are skipped.
#
# Requires: avahi-utils  (sudo apt-get install -y avahi-utils)

set -euo pipefail

OUT="${OUT:-shelly-overview.html}"
MODE="matter"        # matter = CHIP SRP hostname; name = instance prefix; all = everything
NAME_PREFIX=""
OPEN=1

while [ $# -gt 0 ]; do
  case "$1" in
    --all)     MODE="all" ;;
    --name)    MODE="name"; NAME_PREFIX="${2:-}"; shift ;;
    --no-open) OPEN=0 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

if ! command -v avahi-browse >/dev/null 2>&1; then
  echo "avahi-browse missing. Install with: sudo apt-get install -y avahi-utils" >&2
  exit 1
fi

echo "Scanning for _http._tcp modules (5 s)..." >&2

# -r resolve, -p parseable, -t terminate, -k no colours.
# Resolved records start with '=' and are ';'-separated:
#   =;iface;proto;name;type;domain;hostname;address;port;txt
RAW="$(timeout 6 avahi-browse -rptk _http._tcp 2>/dev/null || true)"

rows=""
count=0
seen=";"   # dedup marker: tracks which instance names were already emitted
# avahi resolves the same service per interface AND per protocol, so the same
# module usually arrives multiple times; emit one row per unique name (it does
# not matter which IPv4/IPv6 record we keep, they point at the same module).
while IFS=';' read -r tag iface proto name type domain hostname address port txt; do
  [ "$tag" = "=" ] || continue
  [ -n "$name" ] || continue
  case "$MODE" in
    matter)
      # Shelly Gen4 Matter modules resolve to a CHIP SRP host: 16 hex chars
      # + ".local" (optional trailing dot). LAN web services do not match.
      [[ "$hostname" =~ ^[0-9A-Fa-f]{16}\.local\.?$ ]] || continue ;;
    name)
      case "$name" in "$NAME_PREFIX"*) ;; *) continue ;; esac ;;
    all) : ;;
  esac
  # avahi escapes spaces as \032 etc. — decode the most common one.
  name="${name//\\032/ }"
  # skip if we already have this name (dedup across interfaces/protocols).
  case "$seen" in *";${name};"*) continue ;; esac
  seen="${seen}${name};"
  # Build URL: wrap IPv6 in [], drop the port when it is 80. Decide by the
  # address itself (a Thread module has only an AAAA record, so even its IPv4
  # avahi record reports the IPv6 OMR address), not by the proto column.
  case "$address" in
    *:*) host_for_url="[$address]" ;;
    *)   host_for_url="$address" ;;
  esac
  if [ "$port" = "80" ] || [ -z "$port" ]; then
    url="http://${host_for_url}/"
    url_host="http://${hostname}/"
  else
    url="http://${host_for_url}:${port}/"
    url_host="http://${hostname}:${port}/"
  fi
  rows+="<tr><td><a href=\"${url}\">${name}</a></td>"
  rows+="<td><a href=\"${url_host}\">${hostname}</a></td>"
  rows+="<td class=\"mono\">${address}</td><td>${port}</td></tr>"$'\n'
  count=$((count+1))
done <<< "$RAW"

if [ "$count" -eq 0 ]; then
  rows="<tr><td colspan=\"4\" style=\"text-align:center;color:#888\">No modules found. Is the module powered on and on the same LAN/Thread network?</td></tr>"
fi

cat > "$OUT" <<HTML
<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Shelly Gen4 Matter modules</title>
<style>
  body{font-family:system-ui,Segoe UI,Roboto,sans-serif;margin:2rem;background:#f6f7f9;color:#1b1f24}
  h1{font-size:1.3rem}
  .meta{color:#666;font-size:.85rem;margin-bottom:1rem}
  table{border-collapse:collapse;width:100%;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.08);border-radius:8px;overflow:hidden}
  th,td{padding:.6rem .8rem;text-align:left;border-bottom:1px solid #eee}
  th{background:#0b5fff;color:#fff;font-weight:600}
  tr:hover td{background:#f0f5ff}
  a{color:#0b5fff;text-decoration:none;font-weight:600}
  a:hover{text-decoration:underline}
  .mono{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.85rem;color:#555}
</style></head><body>
<h1>Shelly Gen4 Matter modules</h1>
<div class="meta">Scanned on $(date '+%Y-%m-%d %H:%M:%S') — ${count} module(s) via _http._tcp</div>
<table>
<thead><tr><th>Name (hostname)</th><th>.local</th><th>Address</th><th>Port</th></tr></thead>
<tbody>
${rows}
</tbody></table>
<p class="meta">Refresh: run <code>./shelly-overview.sh</code> again.</p>
</body></html>
HTML

echo "Overview written to: $OUT  (${count} module(s))" >&2

if [ "$OPEN" -eq 1 ]; then
  ( xdg-open "$OUT" >/dev/null 2>&1 || open "$OUT" >/dev/null 2>&1 || true ) &
fi
