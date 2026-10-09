#!/usr/bin/env python3
"""Unit tests for tools/claude-relay/relay.py: file tools, model routing, system prompt, and the "M <model>" line the
Pi's prompt shows ("Claude Sonnet 5.5 $ "). No network."""
import importlib.util, os, pathlib, re, sys, tempfile, types, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
spec = importlib.util.spec_from_file_location("relay", os.path.join(HERE, "..", "claude-relay", "relay.py"))
relay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(relay)


class Tools(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="jt-relay-")
        self.d = os.path.join(self.tmp.name, "files")
        os.mkdir(self.d)
        for n, t in (("a.txt", "hello"), (".secret", "no"), ("big.txt", "x" * 30000)):
            with open(os.path.join(self.d, n), "w") as f: f.write(t)
        with open(os.path.join(self.tmp.name, "outside.txt"), "w") as f: f.write("outside")
        os.symlink(os.path.join(self.tmp.name, "outside.txt"), os.path.join(self.d, "link.txt"))
        os.mkdir(os.path.join(self.d, "sub"))
        self._old, relay.FILES_DIR = relay.FILES_DIR, self.d

    def tearDown(self):
        relay.FILES_DIR = self._old
        self.tmp.cleanup()

    def read(self, n):
        return relay.run_tool("read_file", {"name": n})

    def test_list(self):
        self.assertEqual(relay.run_tool("list_files", {}).split("\n"), ["a.txt", "big.txt"])

    def test_read_ok_and_cap(self):
        self.assertEqual(self.read("a.txt"), "hello")
        self.assertEqual(len(self.read("big.txt")), 20000)

    def test_read_refused(self):
        bad = ["../outside.txt", "../files/a.txt", self.tmp.name + "/outside.txt", os.path.join(self.d, "a.txt"),
               ".secret", "link.txt", "sub", "nope.txt", "", "a.txt/", "./a.txt"]
        for n in bad:
            self.assertTrue(self.read(n).startswith("No such file"), n)
        for a in (None, [], "a.txt", {}, {"name": 5}):
            self.assertTrue(relay.run_tool("read_file", a).startswith("No such file"), repr(a))

    def test_unknown_and_missing_dir(self):
        self.assertEqual(relay.run_tool("rm", {}), "Unknown tool.")
        relay.FILES_DIR = os.path.join(self.tmp.name, "gone")
        self.assertIn("does not exist", relay.run_tool("list_files", {}))


class Routing(unittest.TestCase):
    cfg = types.SimpleNamespace(api_model="haiku", api_model_hard="sonnet", api_model_top="opus")

    def test_routes(self):
        for p, m in (("hi", "haiku"), ("what time is it?", "haiku"),
                     ("Why is the sky blue", "sonnet"), ("fix my script", "sonnet"), ("a" * 281, "sonnet"),
                     ("a" * 280, "haiku"),
                     ("Review the SECURITY of this", "opus"), ("design a kernel", "opus"), ("prove it", "opus")):
            self.assertEqual(relay.pick_model(self.cfg, p), m, p)


class Prompt(unittest.TestCase):
    def test_actions(self):
        self.assertIn("[[note TEXT]]", relay.SAMANTHA)
        self.assertIn("[[led blink]]", relay.SAMANTHA)


class ModelName(unittest.TestCase):
    """The relay tells the Pi which model answered; ask.c shows it as the prompt, "Claude Sonnet 5.5 $ "."""

    def test_display_name(self):
        for m, n in (("claude-haiku-5-5", "Claude Haiku 5.5"), ("claude-sonnet-5-5", "Claude Sonnet 5.5"),
                     ("claude-opus-5-5", "Claude Opus 5.5"), ("claude-opus-4-1", "Claude Opus 4.1")):
            self.assertEqual(relay.display_name(m), n, m)
        for m in ("", None, "sonnet", "gpt-5", "claude-opus-5-5-20260101", "claude-x y-1-2", "claude-opus-5"):
            self.assertEqual(relay.display_name(m), "Claude", repr(m))

    def test_api_reply_names_the_routed_model(self):
        tmp = tempfile.TemporaryDirectory(prefix="jt-relay-")
        key = os.path.join(tmp.name, "key")
        with open(key, "w") as f: f.write("not-a-real-key")
        cfg = types.SimpleNamespace(api_key_file=key, api_model="claude-haiku-5-5", api_model_hard="claude-sonnet-5-5",
                                    api_model_top="claude-opus-5-5", timeout=5)
        seen = []
        def fake(cfg_, key_, model, messages, system=None, effort="medium"):
            seen.append(model)
            return {"stop_reason": "end_turn", "content": [{"type": "text", "text": "hello"}]}
        old, relay.api_call = relay.api_call, fake
        try:
            for q, name in (("hi", "Claude Haiku 5.5"), ("why is it slow", "Claude Sonnet 5.5"),
                            ("security review", "Claude Opus 5.5")):
                st, text, used = relay.run_api(cfg, q)
                self.assertEqual((st, text, used), (200, "hello", name), q)
        finally:
            relay.api_call = old
            tmp.cleanup()
        self.assertEqual(seen, ["claude-haiku-5-5", "claude-sonnet-5-5", "claude-opus-5-5"])

    def test_pi_default_matches_relay_default(self):
        """ask.c's prompt before any answer must name the relay's default (--api-model) model."""
        src = pathlib.Path(ROOT, "arch", "arm64", "ask.c").read_text()
        m = re.search(r'#define ASK_DEFAULT_MODEL "([^"]+)"', src)
        default = re.search(r'"--api-model", default="([^"]+)"', pathlib.Path(spec.origin).read_text()).group(1)
        self.assertTrue(m, "ask.c has no ASK_DEFAULT_MODEL")
        self.assertEqual(m.group(1), relay.display_name(default))
        self.assertIn("prompt[k++] = ' '; prompt[k++] = '$'; prompt[k++] = ' ';", src, "the prompt no longer ends ' $ '")
        self.assertNotIn('"ask> "', src)


if __name__ == "__main__":
    unittest.main(verbosity=0)
