# NovelNow

An AI-assisted novel writing tool for Windows, built in C with a WebView2-powered WebUI.

**Languages**: English (default) · [简体中文](README.zh-TW.md) · 繁體中文 · 日本語 — switch anytime from the sidebar selector; the choice is remembered locally.

> **This is an early preview release — many features are not yet implemented.** I'm developing it toward a project that can bring your ideas to life and let you dive into streaming reading at any time. This may require an AI with long-context processing and high token output speed to power it; otherwise, you'll only experience its basic writing capabilities. **This project is licensed under the GPLv3 license — please take note.**

## Features

- AI-powered chapter writing and expansion, with streaming output
- Context memory compression: per-chapter summaries and foreshadowing tracking injected into prompts
- Foreshadowing registry to keep track of planted and resolved plot threads
- Instant cancel: aborts in-flight AI requests at any moment
- Works with any OpenAI-compatible API — includes a preset for OpenRouter free models

## Getting started

### Run the prebuilt binary

1. Download the latest release zip and extract it
2. Rename `config.default.json` to `config.json` and fill in your API key
3. Run `novelnow.exe` (Windows 10+ with WebView2 Runtime, preinstalled on most systems)

### Build from source

Requirements:

- Visual Studio 2022 with the C/C++ toolset (MSVC)
- Windows SDK

Steps:

```bat
git clone https://github.com/ALexanderLee-real/NovelNow.git
cd NovelNow
build_msvc.bat
```

The script locates VS2022 via `vswhere`, sets up `vcvars64`, and links against the bundled WebView2 loader (`third_party/webview2/x64`).

## Project layout

```
├── src/          C sources (engine, AI API client, project store, host)
├── include/      Internal headers (cJSON, WebView2 bindings)
├── ui/           Frontend assets (HTML/CSS/JS) served inside WebView2
├── third_party/  WebView2 SDK (headers + loader)
├── build_msvc.bat  One-shot MSVC build script
└── config.default.json  Configuration template
```

## Configuration

Copy `config.default.json` to `config.json` (next to the executable) and adjust:

| Field | Description |
|---|---|
| `ai.api_url` | OpenAI-compatible endpoint, e.g. `https://api.deepseek.com` |
| `ai.api_key` | Your API key (kept local, never uploaded) |
| `ai.model` | Model ID, or pick from the OpenRouter free preset |

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).
