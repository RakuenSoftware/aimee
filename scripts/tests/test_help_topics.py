"""Every topic `aimee get-help` advertises must resolve to a docs/agent.md section.

The usage text once listed topics (work queue, git) the help document had no
section for, so `aimee get-help git` answered "unknown topic". Matching mirrors
tool_get_help in src/server/server_mcp.c: lowercase, '-'/'_' as spaces, and a
section matches when either string contains the other.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def norm(s: str) -> str:
    return s.lower().replace("-", " ").replace("_", " ")


def sections() -> list[str]:
    doc = (ROOT / "docs/agent.md").read_text()
    return [norm(line[3:]) for line in doc.splitlines() if line.startswith("## ")]


def advertised_topics() -> list[str]:
    defs = (ROOT / "src/server/cli_command_defs_data.h").read_text()
    entry = re.search(r'\{"get-help",.*?\},', defs, re.S)
    assert entry, "get-help entry not found in cli_command_defs_data.h"
    usage = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', entry.group(0)))
    usage = usage.replace("\\n", " ")
    listed = re.search(r"Explain a topic --(.*?)\. The words are joined", usage, re.S)
    assert listed, "topic list not found in get-help usage"
    return [norm(" ".join(t.split())) for t in listed.group(1).split(",") if t.strip()]


class HelpTopicsTest(unittest.TestCase):
    def test_every_advertised_topic_has_a_section(self) -> None:
        secs = sections()
        topics = advertised_topics()
        self.assertGreater(len(topics), 3)
        for topic in topics:
            with self.subTest(topic=topic):
                self.assertTrue(any(topic in s or s in topic for s in secs),
                                f"'{topic}' matches no section of docs/agent.md: {secs}")


if __name__ == "__main__":
    unittest.main()
