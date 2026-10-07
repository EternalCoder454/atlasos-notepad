#!/usr/bin/env python3
"""Writes bench/sample-50k.md: about 50 KB of everyday Markdown (headings,
paragraphs with inline syntax, nested lists, tasks, quotes, code blocks), the
same every time, for `telamon-notepad --bench`."""
import random
from pathlib import Path

random.seed(4)
words = ("the a notes system update file editor window line text format list task code link page review build test "
         "release folder settings quick simple plain fast markdown heading quote table backup sync change version "
         "user screen menu shortcut theme").split()

def sentence():
    out = []
    for _ in range(random.randint(8, 18)):
        w = random.choice(words)
        r = random.random()
        if r < 0.05:
            w = f"**{w}**"
        elif r < 0.09:
            w = f"*{w}*"
        elif r < 0.11:
            w = f"`{w}()`"
        elif r < 0.12:
            w = f"[{w}](https://example.org/{w})"
        elif r < 0.125:
            w = f"~~{w}~~"
        out.append(w)
    s = " ".join(out)
    return s[0].upper() + s[1:] + "."

parts = []
size = 0
section = 0
while size < 50_000:
    section += 1
    block = [f"## Section {section}: {random.choice(words)} {random.choice(words)}", ""]
    for _ in range(random.randint(1, 3)):
        block += [" ".join(sentence() for _ in range(random.randint(2, 5))), ""]
    kind = section % 5
    if kind == 0:
        for i in range(random.randint(3, 6)):
            block.append(f"- {sentence()}")
            if i % 2:
                block.append(f"  - {sentence()}")
        block.append("")
    elif kind == 1:
        for i in range(random.randint(3, 6)):
            block.append(f"- [{'x' if i % 3 == 0 else ' '}] {sentence()}")
        block.append("")
    elif kind == 2:
        block += ["```rust", "fn main() {", '    println!("hello **not bold**");', "}", "```", ""]
    elif kind == 3:
        block += [f"> {sentence()}", f"> {sentence()}", ""]
    else:
        for i in range(random.randint(3, 5)):
            block.append(f"{i + 1}. {sentence()}")
        block += ["", "---", ""]
    text = "\n".join(block) + "\n"
    parts.append(text)
    size += len(text.encode())

Path(__file__).with_name("sample-50k.md").write_text("# Benchmark notes\n\n" + "".join(parts))
