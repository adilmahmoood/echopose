# EchoPose prompts for Codex (OpenAI CLI)

This directory mirrors the Claude Code `echopose` plugin's operator commands as Codex prompts, plus an `AGENTS.md` carrying the EchoPose project rules.

## Contents

| File | Purpose |
|------|---------|
| `AGENTS.md` | Project rules — repo layout, hard rules, build/test, ESP32 firmware on Windows, witness verification |
| `prompts/echopose-start.md` | Onboarding — Docker demo / repo build / live ESP32 |
| `prompts/echopose-flash.md` | Build + flash ESP32 firmware (8MB / 4MB) |
| `prompts/echopose-provision.md` | Provision WiFi creds + sink IP + channel/MAC overrides |
| `prompts/echopose-app.md` | Run a sensing application (presence / vitals / pose / sleep / MAT / point cloud) |
| `prompts/echopose-train.md` | Train / evaluate / publish a model (incl. GPU on GCloud) |
| `prompts/echopose-advanced.md` | Multistatic / tomography / cross-viewpoint / field-model / mesh-security |
| `prompts/echopose-verify.md` | Run the trust pipeline + pre-merge checklist |

Prompt parity with the Claude Code plugin is enforced by `plugins/echopose/scripts/smoke.sh` (every `commands/<name>.md` must have a matching `codex/prompts/<name>.md`).

## Install

**Per-user prompts** — copy the prompt files into Codex's prompt directory:

```bash
mkdir -p ~/.codex/prompts
cp plugins/echopose/codex/prompts/*.md ~/.codex/prompts/
# now in the codex TUI:  /echopose-start   /echopose-flash   /echopose-app   /echopose-train   /echopose-verify   /echopose-advanced
```

**Project rules** — point Codex at the `AGENTS.md`. Codex auto-discovers an `AGENTS.md` at the repo root and in the working directory; either symlink it or copy it:

```bash
ln -s plugins/echopose/codex/AGENTS.md AGENTS.md          # repo root (if you don't already have one)
# — or, if a root AGENTS.md exists, append the relevant sections from plugins/echopose/codex/AGENTS.md
```

**Config (optional)** — to keep prompts in-repo instead of `~/.codex/prompts`, add to `~/.codex/config.toml`:

```toml
# Codex reads prompts from ~/.codex/prompts by default; symlinking keeps them versioned with the repo:
#   ln -s "$PWD/plugins/echopose/codex/prompts" ~/.codex/prompts/echopose   (then prompts appear as /echopose/echopose-start, etc.)
```

## Notes

- The Codex mirror is the **operator-facing subset** — the seven `/echopose-*` commands. The Claude Code plugin additionally ships skills (`echopose-quickstart`, `echopose-hardware-setup`, `echopose-configure`, `echopose-applications`, `echopose-model-training`, `echopose-advanced-sensing`, `echopose-cli-api`, `echopose-mmwave`, `echopose-verify`) and agents (`echopose-onboarding-guide`, `echopose-config-engineer`, `echopose-training-engineer`) that have no Codex equivalent — their content is folded into `AGENTS.md` and the prompt files.
- On Windows, ESP-IDF firmware builds go through the Python-subprocess pattern documented in `CLAUDE.local.md` (Git Bash / MSYS2 is not supported by ESP-IDF v5.4). Default ESP32 serial port: **COM8**.
