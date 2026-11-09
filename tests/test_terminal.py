#!/usr/bin/env python3
"""Host regression tests using a small Term49-style screen/scrollback model.

No pip packages needed. Implements the ANSI subset emitted by terminal.c,
including Term49's cursor-save clamping and resize anchoring.
"""
import os
from pathlib import Path
import pty
import re
import select
import subprocess
import tempfile
import unicodedata

ROOT = Path(__file__).resolve().parents[1]


class Screen:
    def __init__(self):
        self.rows, self.cols = 6, 24
        self.lines = [[" "] * self.cols for _ in range(self.rows)]
        self.styles = [[False] * self.cols for _ in range(self.rows)]
        self.history = []
        self.row = self.col = 0
        self.inverse = False
        self.saved = (0, 0, False)
        self.footer_checks = 0
        self.footers = []

    def resize(self, rows, cols):
        # Term49's top_line follows the cursor on shrink and reveals history
        # above it on expansion. Cells below a shrinking screen are erased.
        if rows < self.rows:
            shift = max(0, self.row - rows + 1)
            for _ in range(shift):
                self.history.append(self.lines.pop(0))
                self.styles.pop(0)
            self.row -= shift
            self.lines = self.lines[:rows]
            self.styles = self.styles[:rows]
        else:
            count = min(rows - self.rows, len(self.history))
            reveal = self.history[-count:] if count else []
            if count:
                del self.history[-count:]
            self.lines = reveal + self.lines
            self.styles = [[False] * self.cols for _ in reveal] + self.styles
            self.row += count
        while len(self.lines) < rows:
            self.lines.append([" "] * self.cols)
            self.styles.append([False] * self.cols)
        self.lines = [(line + [" "] * cols)[:cols] for line in self.lines]
        self.styles = [(line + [False] * cols)[:cols] for line in self.styles]
        self.rows, self.cols = rows, cols
        self.col = min(self.col, cols - 1)

    def newline(self):
        if self.row == self.rows - 1:
            self.history.append(self.lines.pop(0))
            self.styles.pop(0)
            self.lines.append([" "] * self.cols)
            self.styles.append([False] * self.cols)
        else:
            self.row += 1

    def feed(self, text):
        i = 0
        while i < len(text):
            ch = text[i]
            if ch == "\x1b":
                if text.startswith("\x1b]resize;", i):
                    end = text.index("\x07", i)
                    rows, cols = map(int, text[i + 9:end].split(";"))
                    self.resize(rows, cols)
                    i = end + 1
                    continue
                if text[i + 1] in "78":
                    if text[i + 1] == "7":
                        self.saved = (self.row, min(self.col, self.cols - 1), self.inverse)
                    else:
                        self.row, self.col, self.inverse = self.saved
                    i += 2
                    continue
                match = re.match(r"\x1b\[([?;\d]*)([A-Za-z])", text[i:])
                assert match, repr(text[i:i + 30])
                args, final = match.groups()
                nums = [int(v) if v else 0 for v in args.lstrip("?").split(";")]
                if final == "H":
                    self.row = (nums[0] or 1) - 1
                    self.col = ((nums[1] if len(nums) > 1 else 1) or 1) - 1
                elif final == "m":
                    for n in nums:
                        if n == 0:
                            self.inverse = False
                        elif n == 7:
                            self.inverse = True
                elif final == "K":
                    start = 0 if nums[0] == 2 else self.col
                    for c in range(start, self.cols):
                        self.lines[self.row][c] = " "
                        self.styles[self.row][c] = self.inverse
                elif final == "J":
                    self.lines = [[" "] * self.cols for _ in range(self.rows)]
                elif final == "l" and args == "?2026":
                    footer = "".join(self.lines[-1])
                    assert " | " in footer and " / " in footer, footer
                    assert all(self.styles[-1]), "footer must cover EVERY column"
                    self.footer_checks += 1
                    self.footers.append(footer.strip())
                i += len(match[0])
                continue
            if ch == "\r":
                self.col = 0
            elif ch == "\n":
                self.newline()
            elif ch == "\b":
                self.col = max(0, self.col - 1)
            elif ord(ch) >= 32:
                width = 2 if unicodedata.east_asian_width(ch) in "WF" else 1
                if self.col + width > self.cols:
                    self.newline()
                    self.col = 0
                self.lines[self.row][self.col] = ch
                self.styles[self.row][self.col] = self.inverse
                if width == 2:
                    self.lines[self.row][self.col + 1] = ""
                    self.styles[self.row][self.col + 1] = self.inverse
                self.col += width
            i += 1


def capture(binary, scenario):
    master, slave = pty.openpty()
    proc = subprocess.Popen([str(binary), scenario], stdout=slave,
                            stdin=slave if scenario in ("prompt", "prompt-bold") else subprocess.DEVNULL)
    os.close(slave)
    chunks = []
    sent = False
    entered = False
    while True:
        if not select.select([master], [], [], 10)[0]:
            proc.kill()
            raise AssertionError("terminal driver timed out")
        try:
            data = os.read(master, 65536)
        except OSError:
            break
        if not data:
            break
        chunks.append(data)
        if scenario == "prompt" and not sent and b"> " in b"".join(chunks):
            os.write(master, b"x" * 300 + b"\x01\x05\x15done\r")
            sent = True
        if scenario == "prompt-bold":
            output = b"".join(chunks)
            if not sent and b"> " in output:
                os.write(master, b"z")
                sent = True
            # Do not press Enter until the FIRST typed character has already
            # been emitted in bold: an Enter-triggered redraw cannot pass.
            if sent and not entered and b"\x1b[1mz\x1b[0m" in output:
                os.write(master, b"\r")
                entered = True
    os.close(master)
    assert proc.wait() == 0
    # PTY ONLCR can turn explicit CRLF into CRCRLF, harmless to a terminal.
    return b"".join(chunks).decode("utf-8")


def main():
    with tempfile.TemporaryDirectory(prefix="berrycode-terminal-", dir="/tmp/opencode") as tmp:
        binary = Path(tmp) / "driver"
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-pthread",
                        "-I", str(ROOT), str(ROOT / "tests/terminal_driver.c"),
                        str(ROOT / "terminal.c"), str(ROOT / "lineedit.c"),
                        str(ROOT / "history.c"), str(ROOT / "json.c"),
                        str(ROOT / "util.c"), "-o", str(binary)], check=True)
        for scenario in ("lines", "wrap", "utf8", "edit", "resize", "resize-empty", "prompt", "prompt-bold", "threads", "history", "history-chat", "tokens"):
            text = capture(binary, scenario)
            # Inspect the alternate screen before it is restored on exit.
            text = text[:text.index("\x1b[?1049l")]
            screen = Screen()
            screen.feed(text)
            content = ["".join(line).rstrip() for line in screen.history + screen.lines[:-1]]
            assert not any("test | " in line for line in content), "footer polluted history"
            if scenario == "lines":
                assert [line for line in content if line] == [f"line{i:03}" for i in range(100)]
            elif scenario == "wrap":
                assert [line for line in content if line] == [f"{i:024}" for i in range(40)]
            elif scenario == "utf8":
                assert [line for line in content if line] == ["01234567890123456789012", "界!"] * 20
            elif scenario == "edit":
                assert content[-1] == "> wor!d", content[-1]
            elif scenario == "resize":
                expected = ([f"before{i:02}" for i in range(10)] +
                            [f"middle{i:02}" for i in range(8)] + [f"after{i:02}" for i in range(8)])
                assert [line for line in content if line] == expected, content
            elif scenario == "prompt":
                assert [line for line in content if re.fullmatch(r"line\d\d", line)] == [f"line{i:02}" for i in range(10)]
                assert "result:done" in content, content
            elif scenario == "prompt-bold":
                assert "result:z" in content, content
            elif scenario == "resize-empty":
                assert [line for line in content if line] == ["hello", "world"], content
            elif scenario == "threads":
                assert sorted(line for line in content if line) == [f"{name}{i:03}" for name in "AB" for i in range(100)]
            elif scenario in ("history", "history-chat"):
                color = "35" if scenario == "history-chat" else "32"
                assert f"\x1b[1m\x1b[{color}m> \x1b[0m" in text
                assert "\x1b[1muser00" in text
                assert "\x1b[1mimported text" in text
                expected = [text for i in range(30) for text in (f"> user{i:02}", f"reply{i:02}")]
                assert [line for line in content if re.fullmatch(r"> user\d\d|reply\d\d", line)] == expected
                transcript = "\n".join(content)
                assert "SECRET_SYSTEM_PROMPT" not in transcript
                assert "[tool call] bash" in transcript
                assert "DO_NOT_EXECUTE" in transcript
                assert "[tool result]" in transcript and "safe output" in transcript
                assert "> imported text" in transcript
            elif scenario == "tokens":
                for expected in ("test | 999 / 1000", "test | 1K / 1000K", "test | 1000K / 1M",
                                 "test | 12.3K / 5.7M"):
                    assert expected in screen.footers, screen.footers
                assert screen.footers[-1].endswith("| 12.3K / 5.7M"), screen.footers[-1]
                assert "…" in screen.footers[-1], "long model must truncate before counts"
            assert screen.footer_checks > 0
            print(f"PASS {scenario}: fixed full-width footer, clean scrollback")
        plain = subprocess.check_output([str(binary), "lines"])
        assert plain == "".join(f"line{i:03}\n" for i in range(100)).encode()
        print("PASS redirected output: no terminal escapes")


if __name__ == "__main__":
    main()
