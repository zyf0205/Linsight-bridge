# CLAUDE.md

Guidance for the LinSight web dashboard (`host/web/index.html`). Read the host
repo root `CLAUDE.md` first for daemon, protocol, and bench context.

## What this is

- Single self-contained page: inline CSS and vanilla JS, no framework, no build
  step, no package manager, no external requests (fonts/CDN/analytics).
- Served by `linsightd` from `--web-dir`; when running as the systemd service
  the installed copy is `/usr/local/share/linsight/web/index.html`.
- The page is loaded once at daemon startup. After editing: reinstall
  (`sudo cmake --install build`) and `sudo systemctl restart linsightd`, or run
  the daemon from `host/` with `--web-dir web` for a quick loop.
- Target current Chromium/Firefox/Safari. Use `fetch`, `EventSource`, canvas,
  and `requestAnimationFrame`; no experimental APIs.

## Data contract (do not invent fields)

- `GET /api/stats` once per second: JSON snapshot with `version`, `device`,
  `device_id`, `boot`, `records`, `written`, `dropped`, `log`, `log_error`,
  `stats{...}` (firmware v2.1 STATS fields), and `info`.
- `GET /api/live`: Server-Sent Events; each `data:` line is a JSON array of up
  to 64 records:
  `{"i":id,"f":flags,"d":dlc,"b":"DATAHEX","t":ts_ms,"s":seq}`.
  Flags: bit0 extended, bit1 RTR, bit2 error frame. Events are best-effort —
  the daemon drops them for slow clients instead of blocking CAN RX; never
  assume a complete or gap-free stream.
- `GET /api/control?cmd=...`: whitelist `pause`, `resume`, `clear`,
  `config bitrate=...`, and `config mode=...`. The HTTP reply only means the
  command was accepted; the device ACK arrives as the next `/api/stats`.
- Display only real device traffic. Do not add fake frames, client-side
  generators, or a simulator hook.

## Hard constraints

- Never add a build step, framework, bundler, or external asset. The dashboard
  stays one file that the daemon can serve directly.
- Keep the downstream path non-blocking: ingest into a bounded buffer and
  render in `requestAnimationFrame`; never render per SSE event.
- Keep the memory caps: 900 rows in memory, 30 rows visible, 512 tracked IDs,
  60 sparkline samples. They bound multi-hour sessions.
- One SSE connection and one 1 Hz stats poll. Do not add polling loops or
  duplicate streams.
- No `localStorage`/`sessionStorage`/IndexedDB; page state is per-tab and resets
  on reload. Anything durable belongs on the daemon side.
- HTML injection: table and inspector values are numeric or hex. Device-provided
  strings (`log`, `device_id`, `info`) must go through `textContent` or be
  escaped before `innerHTML`.
- Controls must match `web_control()` in `host/src/main.c`. Change the server
  whitelist first; the page cannot create new daemon capabilities.
- Keep the HTTP bind on loopback (`--http-bind 127.0.0.1`) by default; the page
  has no authentication. Use `0.0.0.0` only on an isolated bench.
- Keep keyboard behavior: `/` focuses the filter, Space toggles freeze, and
  shortcuts must not fire while typing in an input.
- CSV export is generated client-side from the in-memory rows; do not upload
  data anywhere.
- Styling: dark theme, tokens in `:root`; reuse existing classes instead of
  adding a second visual system.

## Verification

- `curl -s http://127.0.0.1:8080/api/stats` and `/api/live` for the contract.
- Open the page: `DEVICE OK` + `SSE live` when the device is connected.
- Drive real traffic with `cangen can0 -n 500 -g 20 -I 0x123 -L 8` (device in
  NORMAL on a two-node bench) and confirm rows, IDs, and counters move.
- Restart the daemon after edits; it serves the installed `index.html`, not the
  repo file.
- There are no web unit tests; the manual checks above are the gate.
