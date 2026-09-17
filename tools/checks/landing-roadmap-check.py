#!/usr/bin/env python3
"""Regression checks for build-time roadmap copy; no repository files mutated."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("landing_roadmap", ROOT / "tools/gen/landing-roadmap.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)

QUEUE = "## Session task queue, intelligently ordered (Sep 2026)\n"
PAGE = "before" + generator.START + "<p>old</p>" + generator.END + "after"


class RoadmapTests(unittest.TestCase):
    def test_open_order_limit_and_section_boundary(self):
        roadmap = QUEUE + """
1. ~~**Already shipped**~~ - old work.
2. **First task** - private diagnostic detail.
3. **Second task** - explanation.
4. **Third task** - explanation.
5. **Fourth task** - explanation.
## Another section
1. **Outside task**
"""
        self.assertEqual(generator.summarize(roadmap), "On the roadmap: First task; Second task; Third task.")

    def test_completion_and_new_work_change_copy(self):
        roadmap = QUEUE + "1. **First task** - details.\n2. **Second task**\n"
        before = generator.render(roadmap, PAGE)
        after = generator.render(roadmap.replace("**First task**", "~~**First task**~~"), PAGE)
        self.assertNotEqual(before, after)
        self.assertNotIn("First task", after)
        self.assertIn("Second task", after)
        self.assertIn("New task", generator.render(roadmap + "3. **New task**\n", PAGE))

    def test_escape_and_preserve_surrounding_markup(self):
        rendered = generator.render(QUEUE + '1. **`A` < B & C**\n', PAGE)
        self.assertEqual(rendered, 'before' + generator.START + '<p>On the roadmap: A &lt; B &amp; C.</p>' + generator.END + 'after')
        self.assertEqual(generator.render(QUEUE + '1. **`A` < B & C**\n', rendered), rendered)

    def test_empty_queue_and_missing_section(self):
        self.assertEqual(generator.summarize(QUEUE + '1. ~~**Done**~~\n'), 'The current task queue is complete. More plans soon.')
        with self.assertRaises(ValueError):
            generator.summarize('No queue here')

    def test_missing_or_duplicate_markers_fail(self):
        for page in ('no markers', PAGE + PAGE, generator.END + generator.START):
            with self.assertRaises(ValueError):
                generator.render(QUEUE, page)

    def test_real_page_can_be_regenerated(self):
        roadmap = (ROOT / 'roadmap.md').read_text()
        page = (ROOT / 'landing/index.html').read_text()
        updated = generator.render(roadmap, page)
        self.assertIn(generator.summarize(roadmap), updated)
        self.assertEqual(generator.render(roadmap, updated), updated)


if __name__ == '__main__':
    unittest.main()
