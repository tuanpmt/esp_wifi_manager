# Changelog

## 2.0.0

### Breaking changes

- `wifi_manager_deinit()` now takes `bool deinit_wifi` (#7). Pass `true` for the
  previous behavior; `false` keeps WiFi connected and only tears down the
  manager, HTTP handlers, BLE service and mDNS.
- All SSID fields in public structs grew from `char[32]` to `char[33]` so a
  32-byte SSID fits with its terminator (#8, #9): `wifi_network_t`,
  `wifi_status_t`, `wifi_scan_result_t`, `wifi_mgr_ap_config_t`,
  `wifi_ap_status_t`, `wifi_connected_t`, `wifi_disconnected_t`.
  Recompile everything that uses these types. Data saved in NVS by 1.x is
  migrated automatically on first boot.
- `POST /networks` and BLE `add_network` are an upsert (#12): adding an
  existing SSID updates it instead of returning "Already exists". Fields that
  are omitted keep their saved values.
- Scan results no longer include hidden networks (#13, #15).
- `WIFI_MGR_EVT_AP_START` / `WIFI_MGR_EVT_AP_STOP` are emitted once per
  start/stop call instead of on every driver restart (#19).
- Bluedroid no longer releases Bluetooth Classic memory by default (#11).
  Enable `CONFIG_WIFI_MGR_BLE_RELEASE_CLASSIC_BT_MEM` to keep the old behavior.
- `POST /connect` with an SSID that is not saved returns `404` instead of `200`.

### Added

- NimBLE host stack support next to Bluedroid (#9)
- BLE `update_network`, `list_vars`, `del_var` commands, chunked responses
  larger than one MTU, service UUID in advertising data (#12)
- BLE service-only mode when the application already runs the BLE stack
  (#12, experimental, see README)
- `pre_request_hook` for REST API endpoints and `on_before_var_set` variable
  validation callback (#16)
- Web UI serves any extra file from `CONFIG_WIFI_MGR_WEBUI_CUSTOM_PATH` (#16)
- "Hidden network" form in the Web UI to connect by SSID (#13)
- Kconfig options with built-in defaults: max scan results, HTTP body size,
  URI handler count, task stack size and priority (#4)
- HTTP handlers are unregistered from an application-owned httpd on deinit (#6)
- Python HTTP emulator for Web UI development in `tools/test_server` (#18)
- Host unit tests for pure logic: `make -C test/host`

### Fixed

- Fallback network connection was dropped by stale disconnect events from the
  primary network, causing reconnect loops (#14)
- Removing the network the device is connected to now disconnects from it (#10)
- SSIDs of exactly 32 bytes: garbage after the name in scan results (#8), and
  such networks could never be connected to (STA and AP SSID truncated to 31)
- Saved passwords were wiped by upsert/update requests that omitted them
- REST `/networks/:ssid` and `/vars/:key` did not URL-decode the path, so
  networks with spaces or non-ASCII names could not be updated or deleted
- `frontend/dist` was missing from the published component, breaking Web UI
  builds from the registry (#3)
- Web UI wildcard file handler allowed path traversal (`..`), served `.json`
  as JavaScript and ignored files that exist only as `.gz`
- Captive portal: added Windows `/redirect`, unknown paths redirect to the
  portal while the AP is active, warning when `CONFIG_HTTPD_MAX_REQ_HDR_LEN` is
  too small for Android/Chrome probes (#21)
- BLE: commands ran inside the Bluedroid stack task; long (prepared) writes
  were ignored on Bluedroid; deinit could kill the command task mid-command or
  touch a BLE stack it never initialized
- `wifi_manager_deinit()` freed the context while the manager task could still
  be inside the connect sequence
- Auto-reconnect backoff no longer blocks the manager task
- Web UI could not connect to open networks that were not saved yet

### Migrating from 1.x

1. Replace `wifi_manager_deinit()` with `wifi_manager_deinit(true)`.
2. If your code copies SSIDs into the public structs with hard-coded sizes
   (`32`, `31`), use `sizeof(field)` instead.
3. If a client relied on `POST /networks` failing for an existing SSID, check
   with `GET /networks` first.
4. If your firmware uses Bluedroid and needs the RAM previously freed from
   Bluetooth Classic, set `CONFIG_WIFI_MGR_BLE_RELEASE_CLASSIC_BT_MEM=y`.
5. For captive portal deployments add `CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024`
   to `sdkconfig.defaults`.

Thanks to @thorrak and @telesyst for the contributions in this release.
