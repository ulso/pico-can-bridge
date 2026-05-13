# Release checklist

This checklist is intended for manual releases. GitHub Actions can automate the
same flow later.

## 1. Prepare the tree

Make sure the working tree is clean:

```sh
git status --short
```

Review recent changes:

```sh
git log --oneline --decorate -n 10
```

## 2. Confirm the Zephyr patch

This project currently carries a local Zephyr CDC-NCM patch:

```text
patches/zephyr-cdc-ncm-macos-altsetting.patch
```

If the Zephyr workspace has been updated or reset, apply the patch before
building:

```sh
cd <zephyr-workspace>/zephyr-main
git apply <repo>/patches/zephyr-cdc-ncm-macos-altsetting.patch
```

## 3. Build a clean UF2

Run a pristine build from the Zephyr workspace:

```sh
cd <zephyr-workspace>
./venv/bin/west build --pristine \
  -b adafruit_feather_canbus_rp2040 \
  -d <repo>/build \
  <repo>
```

The release UF2 is generated at:

```text
<repo>/build/zephyr/zephyr.uf2
```

Copy or rename it for the release asset, for example:

```text
pico-can-bridge-v0.1.0.uf2
```

## 4. Smoke test

Flash the UF2 to the board and verify:

- The onboard LED blinks during CDC-NCM setup and stays on after link-local IP
  assignment.
- The host can resolve and ping `pico-can-bridge.local`.
- The web UI loads at `http://pico-can-bridge.local/`.
- The UI can connect to the `/can` WebSocket.
- CAN TX works from the UI.
- CAN RX appears in the frame table when another CAN node is transmitting.
- The UI works on at least one desktop host.
- If available, the UI works from iPhone or iPad over USB-C.

Useful commands:

```sh
ping pico-can-bridge.local
curl http://pico-can-bridge.local/
dns-sd -B _http._tcp local
```

## 5. Create the version tag

Use semantic versioning for public releases:

```sh
git tag -a v0.1.0 -m "Release v0.1.0"
git push origin v0.1.0
```

## 6. Create the GitHub release

Create a GitHub Release from the tag and upload:

- `pico-can-bridge-v0.1.0.uf2`
- optional `SHA256SUMS`

Suggested release notes structure:

```md
## Highlights

- USB CDC-NCM CAN bridge for the Adafruit Feather RP2040 CAN Bus board
- Built-in web UI for macOS, iPad, and iPhone
- JSON WebSocket CAN API at `/can`
- mDNS/DNS-SD service discovery

## Known issues

- RP2040 CDC-NCM startup currently depends on a local Zephyr patch.
- macOS may still need reconnect/reset recovery in some USB startup cases.
```

## 7. After release

Update any project pages or posts with:

- GitHub repository URL
- release URL
- screenshots from `docs/images/`
- short project description
