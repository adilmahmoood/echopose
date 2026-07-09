# echopose — Claude Code + Codex plugin for WiFi sensing

End-to-end toolkit for **EchoPose** (WiFi-DensePose): onboarding, ESP32 hardware setup, configuration, sensing applications, model training, advanced multistatic sensing, and witness verification — from practical to advanced.

Part of the **`echopose` marketplace** — manifest at the repo root: `.claude-plugin/marketplace.json` (this plugin's `source` is `./plugins/echopose`).

## Install / test

```bash
# In Claude Code — add this repo as a plugin marketplace, then install:
/plugin marketplace add ruvnet/EchoPose
/plugin install echopose@echopose

# Or try it locally without installing (from a clone of the repo):
claude --plugin-dir ./plugins/echopose
```

For Codex (OpenAI CLI), see [`codex/`](codex/) — all seven `/echopose-*` commands mirrored as Codex prompts, plus an `AGENTS.md` and install instructions in [`codex/README.md`](codex/README.md).

## What's inside

### Skills (auto-discovered from `skills/`)

| Skill | What it does |
|-------|--------------|
| `echopose-quickstart` | Onboarding & first run — Docker demo, repo build, fastest path to a live dashboard |
| `echopose-hardware-setup` | ESP32-S3 / C6 firmware build, flash, WiFi provisioning, serial monitoring |
| `echopose-configure` | sdkconfig variants, NVS provisioning, channel/MAC overrides (ADR-060), edge modules (ADR-041), sensing-server flags, mesh, Cognitum Seed |
| `echopose-applications` | Run presence, vitals, pose (WiFlow), sleep, environment mapping, MAT, point-cloud fusion, novel RF apps |
| `echopose-model-training` | Camera-free pose, camera-supervised pose (92.9% PCK@20, ADR-079), RuVector embeddings (AETHER), domain generalization (MERIDIAN), local SNN, GPU on GCloud, HF publishing |
| `echopose-advanced-sensing` | RuvSense multistatic, cross-viewpoint fusion, RF tomography, persistent field model, intention signals, adversarial detection, mesh security |
| `echopose-cli-api` | `wifi-densepose` CLI binary (incl. MAT subcommands), REST API (`wifi-densepose-api`), browser/WASM (`wifi-densepose-wasm`, `wifi-densepose-wasm-edge`) |
| `echopose-mmwave` | mmWave / FMCW radar — ESP32-C6 + MR60BHA2 (60 GHz HR/BR/presence), HLK-LD2410 (24 GHz), mmWave↔CSI fusion (48-byte fused vitals) |
| `echopose-verify` | Rust tests, deterministic Python proof, firmware hashes, ADR-028 witness bundle + self-verification, pre-merge checklist |

### Commands (`commands/`)

| Command | Purpose |
|---------|---------|
| `/echopose-start` | Get started — pick Docker / build / hardware and walk through it |
| `/echopose-flash` | Build + flash ESP32 firmware (8MB / 4MB), confirm CSI stream |
| `/echopose-provision` | Provision WiFi creds, sink IP, channel / MAC-filter onto a node |
| `/echopose-app` | Run a sensing application |
| `/echopose-train` | Train / evaluate / publish a model (incl. GPU) |
| `/echopose-advanced` | Use multistatic / tomography / cross-viewpoint / mesh-security features |
| `/echopose-verify` | Run the trust pipeline + pre-merge checklist |

### Agents (`agents/`)

| Agent | Role |
|-------|------|
| `echopose-onboarding-guide` | Walks a newcomer from zero to a working setup |
| `echopose-config-engineer` | Sets up / tunes a deployment (firmware, NVS, edge modules, mesh, Seed) |
| `echopose-training-engineer` | Trains, evaluates, and ships models |

## Compatibility

- **Claude Code** — skills, commands, and agents are auto-discovered; no `claude-flow` MCP server required (skills drive EchoPose's own tooling: `cargo`, `python`, `idf.py`, `docker`, `node`). Optional: `npx @claude-flow/cli@latest security scan` is referenced for security changes.
- **Codex (OpenAI CLI)** — workflows mirrored under `codex/prompts/`; drop them in `~/.codex/prompts/` (or point Codex at `codex/`). `codex/AGENTS.md` carries the project rules.
- **Target repo** — assumes the [`ruvnet/EchoPose`](https://github.com/ruvnet/EchoPose) / `wifi-densepose` layout: `v2/crates/`, `firmware/esp32-csi-node/`, `archive/v1/`, `scripts/`, `docs/adr/`. On Windows, ESP-IDF builds go through the Python-subprocess pattern in `CLAUDE.local.md`.

## Namespace coordination

This plugin claims the kebab-case `echopose-*` namespace for its skills, commands, and agents (skills: `echopose-quickstart`, `echopose-hardware-setup`, `echopose-configure`, `echopose-applications`, `echopose-model-training`, `echopose-advanced-sensing`, `echopose-cli-api`, `echopose-mmwave`, `echopose-verify`; commands: `/echopose-start`, `/echopose-flash`, `/echopose-provision`, `/echopose-app`, `/echopose-train`, `/echopose-advanced`, `/echopose-verify`; agents: `echopose-onboarding-guide`, `echopose-config-engineer`, `echopose-training-engineer`). It does not write to any `claude-flow` memory namespace. If combined with the `ruflo` marketplace, defer to `ruflo-agentdb` ADR-0001 §"Namespace convention" — there is no overlap (`echopose-*` vs. `ruflo-*`).

## Verification

```bash
bash plugins/echopose/scripts/smoke.sh
```

Structural contract: plugin.json has `version` + `keywords` and does **not** enumerate skills/commands/agents; every skill/command/agent file exists with valid frontmatter; README has a Compatibility section and a Namespace coordination block; ADR-0001 exists with status `Proposed`; no wildcard tools in skills; Codex mirror present **and parity** — every `commands/<name>.md` has a matching `codex/prompts/<name>.md`.

## Architecture Decisions

- [`docs/adrs/0001-echopose-plugin-contract.md`](docs/adrs/0001-echopose-plugin-contract.md) — plugin contract (Proposed): structure, namespace, compatibility surface, smoke scope, Codex mirror policy.

## Hardware note

`COM8` is the default ESP32 serial port in this plugin's docs — confirmed against an attached **ESP32-S3** (USB VID:PID `303A:1001`, Espressif) running the EchoPose CSI firmware (live `adaptive_ctrl` ticks + `csi_collector: CSI cb #… len=128 …` on the serial monitor). The repo's `CLAUDE.local.md` historically referenced `COM7`; some README snippets reference `COM9`. Always confirm the actual port (`python -c "import serial.tools.list_ports as l; print([p.device for p in l.comports()])"`, or Device Manager) before flashing. On Windows, `provision.py --help` needs `PYTHONUTF8=1` to print (non-ASCII in the help text); the build/flash path goes through the Python-subprocess pattern in `CLAUDE.local.md` (ESP-IDF v5.4 ≠ Git Bash).
