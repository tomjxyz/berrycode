# berrycode — a coding agent for the BlackBerry 10 terminal

berrycode brings an agentic coding assistant to the BlackBerry 10 phone,
running inside the Term49CX terminal. Point it at any OpenAI-compatible
endpoint — OpenAI, OpenRouter, a local Ollama or llama.cpp box — and it
reads your code, answers questions, edits files and runs shell commands.

## Features

- **Agentic loop**: the model uses tools (read, list, glob, grep, write,
  edit, shell) and iterates until the job is done.
- **You stay in control**: file writes and shell commands ask first.
  Enter = yes, `a` = always allow this session, or type an instruction
  to deny with a note the model actually listens to. `--yolo` skips
  asking entirely.
- **Code and chat modes**: `/code` for tools, `/chat` for plain
  conversation. Each mode keeps its own history; `/resume` picks up
  where you left off. `-c` / `--continue` and `/resume` also replay saved
  user messages, replies and tool activity into the terminal scrollback.
  Saved tool calls are displayed, never re-executed. Ctrl-C stops the replay
  without discarding the restored conversation. `-c -p` restores context but
  does not print the old conversation.
- **Markdown replies, rendered live**: **bold**, *italic*, `code`,
  fenced blocks, `# headers`, `>` quotes, `- bullet lists`.
- **Small-screen aware**: output wraps to your columns, long commands and
  results are paged to what fits, CJK counts double-width, pastes land
  as one block, Term49CX colours (including dim text) just work.
- **Persistent full-width footer**: Output enters Term49CX's native scrollback.
  It shows `model | input / output` with cumulative provider-reported token
  counts.
- **OpenCode Go Compatible (not approved)**: sends its own `berrycode/<version>`
  user agent plus a stable per-conversation `x-opencode-session` header (new ID on
  `/clear`, restored on `/resume`, toggleable via `opencode_session`).
- **Lightweight**: one C99 binary, no runtime, no dependencies
  beyond libcurl. Logs to the sandbox `logs/log` for post-mortem
  debugging.

## Quick start

Copy the `berrycode/` folder somewhere Term49 can reach, e.g.
`/accounts/1000/shared/misc/berrycode`, then in Term49:

```sh
cd /accounts/1000/shared/misc/berrycode
chmod +x berrycode
mkdir -p ~/.berrycode && cp berrycode.conf.example ~/.berrycode/config
vi ~/.berrycode/config        # set base_url, api_key, model
./berrycode
```

(Or skip the file: `./berrycode -u URL -k KEY -m MODEL`.)
Keep `cacert.pem` next to the binary — the phone's root certificates
are from ~2015, so berrycode ships a fresh Mozilla bundle for HTTPS.
Point `ca_bundle` elsewhere in the config, or as a last resort set
`insecure = true`.

## What it can do

```sh
./berrycode # Interactive mode
./berrycode "where is DNS configured in this repo?"
./berrycode -p "summarise uncommitted changes"   # one-shot, prints and exits
```

Slash commands: `/help` `/status` `/model NAME` `/models` `/yolo`
`/thinking` `/timeout N` `/set KEY VALUE` `/clear` `/resume` `/cd DIR`
`/chat` `/code` `/quit`.
Prefix a line with `!` to run a shell command yourself; end a line with
`\` to continue it, wrap a block in `"""` lines for multi-line input.
Ctrl-C cancels the current reply, Ctrl-D exits.

Prompt editing: Backspace works across wrapped lines, Left/Right move,
Up/Down recall history, Ctrl-U/Ctrl-K kill to start/end, Ctrl-W deletes
a word, Ctrl-A/Ctrl-E jump to start/end.

The footer is attached to the live terminal screen, not overlaid on the
terminal's historical view when you manually scroll back. History retains
the terminal's configured scrollback limit and the existing alternate-screen
lifetime: quitting restores the shell screen. Long prompt edits use a bounded
viewport. Interactive TTY prompts always use the built-in editor (kernel echo
would bypass footer protection), even with `line_editor = false`.

`!` commands have their stdout/stderr captured and terminal controls removed
so they cannot move or archive the footer. Their stdin is `/dev/null`; run
interactive programs such as editors directly from the shell outside berrycode.

## Configuration (`~/.berrycode/config`)

```ini
base_url = https://api.openai.com/v1
#base_url = https://openrouter.ai/api/v1
#base_url = http://192.168.1.50:11434/v1      # Ollama, plain HTTP
api_key = sk-REPLACE_ME
model = gpt-4o-mini
```

`OPENAI_API_KEY` / `OPENAI_BASE_URL` / `BERRYCODE_MODEL` env vars and
`-m`/`-u`/`-k` flags override the file. See `berrycode.conf.example` for
every knob: timeouts, token limits, temperature, search limits, output
paging, extra headers, TLS.

## Files

| Path                        | Contents                                              |
| --------------------------- | ----------------------------------------------------- |
| `~/.berrycode/config`       | settings                                              |
| `~/.berrycode/session.json` | code-mode conversation (per-conversation ID included) |
| `~/.berrycode/chat.json`    | chat-mode conversation                                |

## Building (for the curious)

Cross-compiled with the BB10 NDK toolchain (GCC 5.4.0, `-std=gnu99`):

```sh
source ~/bbndk/bbndk-env.sh
make -C apps/plain_berrycode clean both   # arm/berrycode + x86/berrycode
make -C apps/plain_berrycode check        # static device checks, must PASS
```

`make dist` bundles the phone binary + `cacert.pem` + sample config
into `berrycode-arm.tar.gz`. Zero warnings on both architectures.

Host-side terminal/scrollback regression tests (Python 3 + a host C compiler,
no extra packages): `python3 apps/plain_berrycode/tests/test_terminal.py`.
Offline continue/resume integration tests:
`python3 apps/plain_berrycode/tests/test_resume.py`.

berrycode 0.1.4. Licensed under the MIT License — see `LICENSE`.
