#!/usr/bin/env python3
"""Offline CLI integration coverage for -c, --chat -c, /resume and -c -p."""
import json
import os
from pathlib import Path
import pty
import select
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="berrycode-resume-", dir="/tmp/opencode") as directory:
        home = Path(directory)
        binary = home / "berrycode"
        sources = ["main.c", "json.c", "util.c", "spinner.c", "lineedit.c", "terminal.c", "history.c", "tests/api_stub.c"]
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(ROOT),
                        *[str(ROOT / source) for source in sources], "-o", str(binary)], check=True)
        data = home / ".berrycode"
        data.mkdir()
        config = data / "config"
        config.write_text("color = false\nanimation = false\nbase_url = http://offline.invalid/v1\n")
        env = {"PATH": os.environ["PATH"], "HOME": str(home), "TERM": "xterm"}

        def seed(chat=False, tokens=(12345, 5678901)):
            messages = [
                {"role": "user", "content": "previous chat question" if chat else "previous code question"},
                {"role": "assistant", "content": "**previous answer**"},
                {"role": "assistant", "content": None,
                 "tool_calls": [{"id": "call1", "type": "function", "function": {"name": "bash", "arguments": "NEVER_EXECUTE"}}]},
                {"role": "tool", "tool_call_id": "call1", "content": "previous tool result"},
            ]
            path = data / ("chat.json" if chat else "session.json")
            session = {"cwd": str(home), "session_id": "test-session-id", "messages": messages}
            if tokens is not None:
                session.update(tokens_in=tokens[0], tokens_out=tokens[1])
            path.write_text(json.dumps(session))
            return path, messages

        def run(args, input="/status\n/quit\n"):
            result = subprocess.run([str(binary), "--config", str(config), *args], input=input,
                                    text=True, capture_output=True, env=env, cwd=home, timeout=10)
            assert result.returncode == 0, result.stderr
            return result.stdout

        def run_tty(args):
            master, slave = pty.openpty()
            process = subprocess.Popen([str(binary), "--config", str(config), *args],
                                       stdin=subprocess.PIPE, stdout=slave, stderr=subprocess.PIPE,
                                       env=env, cwd=home)
            os.close(slave)
            process.stdin.write(b"/quit\n")
            process.stdin.close()
            chunks = []
            while True:
                if not select.select([master], [], [], 10)[0]:
                    process.kill()
                    raise AssertionError("resume TTY test timed out")
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                chunks.append(chunk)
            os.close(master)
            assert process.wait(timeout=10) == 0
            process.stderr.close()
            return b"".join(chunks).decode()

        for args, chat, input in [(["-c"], False, "/status\n/quit\n"), (["--chat", "-c"], True, "/status\n/quit\n"),
                                   ([], False, "/resume\n/status\n/quit\n")]:
            path, messages = seed(chat)
            output = run(args, input)
            question = "previous chat question" if chat else "previous code question"
            assert question in output and "previous answer" in output, output
            assert "**previous answer**" not in output, "saved markdown was not rendered"
            assert "previous tool result" in output and "NEVER_EXECUTE" in output
            assert "previous conversation" not in output
            assert "resumed code session:" not in output and "resumed chat session:" not in output
            assert output.index(question) < output.index("previous answer") < output.index("previous tool result")
            saved = json.loads(path.read_text())
            assert saved["messages"] == messages
            assert saved["session_id"] == "test-session-id"
            assert saved["tokens_in"] == 12345 and saved["tokens_out"] == 5678901
            assert "tokens 12345 in / 5678901 out" in output
            print("PASS " + (" ".join(args) if args else "/resume") + ": replayed history without changing conversation")
        config.write_text("color = true\nanimation = false\nbase_url = http://offline.invalid/v1\n")
        for args, chat, color in [(["-c"], False, "32"), (["--chat", "-c"], True, "35")]:
            seed(chat)
            output = run_tty(args)
            question = "previous chat question" if chat else "previous code question"
            assert f"\x1b[1m\x1b[{color}m> \x1b[0m" in output
            assert f"\x1b[1m{question}" in output
        config.write_text("color = false\nanimation = false\nbase_url = http://offline.invalid/v1\n")
        print("PASS restored prompt styling: chat purple, code green, input bold")
        seed()
        output = run(["-c", "-p", "new question"], "")
        assert "offline test response" in output
        assert "[tokens so far:" not in output
        assert "previous code question" not in output and "previous conversation" not in output
        saved = json.loads((data / "session.json").read_text())
        assert saved["tokens_in"] == 13545 and saved["tokens_out"] == 5679151
        output = run(["-c"])
        assert "tokens 13545 in / 5679151 out" in output
        print("PASS tokens: restored totals accumulate and survive another continue")
        seed(tokens=None)
        output = run(["-c"])
        assert "tokens 0 in / 0 out" in output
        print("PASS legacy sessions: missing usage defaults to zero")
        seed()
        seed(chat=True, tokens=(99, 42))
        output = run(["-c"], "/chat\n/resume\n/status\n/code\n/status\n/clear\n/status\n/quit\n")
        assert "tokens 99 in / 42 out" in output
        assert "tokens 12345 in / 5678901 out" in output
        assert "tokens 0 in / 0 out" in output
        print("PASS tokens: mode-specific totals and /clear reset")
        assert "0.1.4" in run(["--version"], "")
        print("PASS -c -p: history restored without replay on stdout; version 0.1.4")


if __name__ == "__main__":
    main()
