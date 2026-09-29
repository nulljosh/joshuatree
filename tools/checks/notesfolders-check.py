#!/usr/bin/env python3
"""Regression coverage for Notes' v2.0 folders/multiple-notes pass
(kernel/editor.h's gui_launch_editor + the NOTES/ model it drives).

Proves, against a real FAT disk and the real GUI + terminal shell (never
reading kernel memory to shortcut a claim):

  1. Migration: a NOTES.TXT already on disk (the old always-open single
     note) is moved into the default folder as its first note, its
     content intact, and NOTES.TXT itself is gone afterward.
  2. A new note ('n' in the notes browser) lands in the CURRENT folder as
     a real file on disk -- proven with `ls`/`cat` through the terminal's
     own VFS commands, not the GUI's own idea of what it saved.
  3. Both notes survive a reboot: a second, fresh QEMU boot on the same
     disk image still lists them.
  4. Delete asks first: 'd' with Esc at the confirm prompt leaves the
     note file on disk; 'd' then "yes" actually removes it.

Same "serial marker is the assertion, not a screen scrape" idiom
filerobust-check.py/notetest-check.sh use: the terminal's own `cat`
command already serial_puts's "cat <name>: n=<len>" (kernel.c), so a
file's presence and exact byte length are read off that, never off the
GUI's in-RAM buffers.
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.chdir(ROOT)
subprocess.run(["make", "-s", "kernel.elf"], check=True)

WORKDIR = tempfile.mkdtemp(prefix="jt-notesfolders-")
DISK = os.path.join(WORKDIR, "disk.img")
LEGACY = os.path.join(WORKDIR, "legacy_notes.txt")
QMP_PORT = 44821

LEGACY_TEXT = "Migrated note\n\nThis one predates folders.\n"
EDIT_APPEND = "\nplus edited"
SECOND_TEXT = "second note content"

subprocess.run(["bash", "tools/mkdisk.sh", DISK], check=True)
with open(LEGACY, "w") as f:
    f.write(LEGACY_TEXT)
subprocess.run(["mcopy", "-i", DISK, LEGACY, "::NOTES.TXT"], check=True)

fails = []


def fail(msg):
    fails.append(msg)
    print("FAIL: " + msg)


class Machine:
    def __init__(self, disk, extra_args=()):
        self.serial_path = os.path.join(WORKDIR, "serial-%d" % time.time_ns())
        args = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                "-drive", f"file={disk},format=raw,if=ide",
                "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server,nowait",
                "-serial", "file:" + self.serial_path]
        args += list(extra_args)
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.sock = None
        for _ in range(50):
            time.sleep(0.2)
            try:
                self.sock = socket.create_connection(("127.0.0.1", QMP_PORT)); break
            except OSError:
                pass
        if self.sock is None:
            raise AssertionError("QMP never came up")
        self.f = self.sock.makefile("rw")
        self.f.readline()
        self.cmd({"execute": "qmp_capabilities"})
        # Wait for the desktop's own serial marker instead of a fixed 5s:
        # on a loaded runner the boot can take longer than that, and every
        # key sent before the desktop is up is lost.
        for _ in range(600):
            if "guidesktop" in self.serial(): break
            time.sleep(0.1)
        time.sleep(1.0)

    def cmd(self, obj):
        import json
        self.f.write(json.dumps(obj) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                return r

    def move(self, x, y):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / 960)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / 540)}}]}})

    def click(self):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        self.cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
        time.sleep(0.2)

    def monitor(self, line):
        return self.cmd({"execute": "human-monitor-command", "arguments": {"command-line": line}})

    def combo(self, combo):
        """Chords ("ctrl-alt-backspace") go through the human monitor's
        own sendkey, which understands hyphenated combos; QMP's own
        send-key event wants a single qcode, not a combo string."""
        self.monitor(f"sendkey {combo} 30")
        time.sleep(0.1)

    def key(self, qcode):
        if "-" in qcode:  # a chord ("shift-n", "ctrl-s", ...): route to combo()
            self.combo(qcode); return
        self.cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.1)

    def type_text(self, text):
        punctuation = {" ": "spc", "\n": "ret", ".": "dot", ",": "comma"}
        for c in text:
            self.key(punctuation.get(c, ("shift-" + c.lower()) if c.isupper() else c))

    def shell(self, line):
        """Toggles into the text shell if needed is the caller's job
        (ctrl-alt-backspace); this just types one command + Enter."""
        self.type_text(line)
        self.key("ret")
        time.sleep(0.3)

    def serial(self):
        try:
            with open(self.serial_path) as f:
                return f.read()
        except FileNotFoundError:
            return ""

    def quit(self):
        try:
            self.cmd({"execute": "quit"})
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill(); self.proc.wait()


def open_notes(m):
    m.move(458, 487); m.click(); time.sleep(0.5)
    # A dock click opens straight into a note; Ctrl+L reveals the browser.
    m.combo("ctrl-l"); time.sleep(0.4)


def to_shell(m):
    """One-way (kernel.c: Ctrl+Alt+Backspace breaks out of gui_run());
    getting back to the desktop is the shell's own "gui" command."""
    m.combo("ctrl-alt-backspace"); time.sleep(1.0)


def to_gui(m):
    m.shell("gui")
    time.sleep(1.0)


def cat_len(serial_text, occurrence, name):
    """Returns the Nth (0-indexed) `cat <name>: n=<len>` marker's len, or
    None if there aren't that many / it's "not found"."""
    marker = f"cat {name}: "
    hits = [line for line in serial_text.splitlines() if line.startswith(marker)]
    if occurrence >= len(hits):
        return None
    rest = hits[occurrence][len(marker):]
    if rest.startswith("not found"):
        return None
    return int(rest.split("=", 1)[1])


# ---------------------------------------------------------------------
# Boot 1: migration + new note lands in the current folder
# ---------------------------------------------------------------------
m1 = Machine(DISK)
try:
    open_notes(m1)
    # Migration happens the moment Notes first opens (gui_launch_editor's
    # one-time notes_migrate_legacy). The browse screen lands on the
    # notes list with the migrated note already selected; Enter opens it.
    m1.key("ret"); time.sleep(0.5)
    m1.combo("ctrl-end")
    m1.type_text(EDIT_APPEND)
    m1.combo("ctrl-s"); time.sleep(0.5)
    m1.key("esc"); time.sleep(0.2)
    m1.key("esc"); time.sleep(0.3)  # back to browse, then fully closed

    # New note, current folder, opens for editing immediately. The app
    # was just closed (two Escs), so it needs a fresh dock click first.
    open_notes(m1)
    m1.key("n"); time.sleep(0.5)
    m1.type_text(SECOND_TEXT)
    m1.combo("ctrl-s"); time.sleep(0.5)
    m1.key("esc"); time.sleep(0.2)
    m1.key("esc"); time.sleep(0.3)

    to_shell(m1)
    m1.shell("cd NOTES")
    m1.shell("cd NOTES")
    m1.shell("ls")
    m1.shell("cat N0000001.TXT")
    m1.shell("cat N0000002.TXT")
    m1.shell("cd ..")
    m1.shell("cd ..")
    m1.shell("cat NOTES.TXT")

    serial = m1.serial()
    n1 = cat_len(serial, 0, "N0000001.TXT")
    n2 = cat_len(serial, 0, "N0000002.TXT")
    root_notes_txt = cat_len(serial, 0, "NOTES.TXT")

    expected_n1 = len(LEGACY_TEXT) + len(EDIT_APPEND)
    if n1 != expected_n1:
        fail(f"migrated note N0000001.TXT is {n1} bytes, expected {expected_n1} (legacy content + the edit, byte for byte)")
    if n2 != len(SECOND_TEXT):
        fail(f"new note N0000002.TXT (created with 'n') is {n2} bytes on disk, expected {len(SECOND_TEXT)} -- it did not land as a real file in the current folder")
    if root_notes_txt is not None:
        fail("NOTES.TXT still exists at VFS root after migration -- should have moved into NOTES/NOTES/, not been left behind")
    if not fails:
        print("PASS: migration kept the legacy note's content intact, and a new note lands in the current folder as a real file")
finally:
    m1.quit()

# ---------------------------------------------------------------------
# Boot 2: survives reboot, then delete asks first
# ---------------------------------------------------------------------
m2 = Machine(DISK)
try:
    to_shell(m2)
    m2.shell("cd NOTES")
    m2.shell("cd NOTES")
    m2.shell("ls")
    m2.shell("cat N0000001.TXT")
    m2.shell("cat N0000002.TXT")
    serial = m2.serial()
    if cat_len(serial, 0, "N0000001.TXT") != expected_n1:
        fail("N0000001.TXT did not survive a reboot with its content intact")
    if cat_len(serial, 0, "N0000002.TXT") != len(SECOND_TEXT):
        fail("N0000002.TXT did not survive a reboot with its content intact")
    if not fails:
        print("PASS: both notes survive a reboot, same folder, same bytes")

    to_gui(m2)  # a dock click lands straight on the notes browse list
    # (gui_launch_editor resets notes_focus to it every open), so 'd'
    # reaches the delete confirm directly, no Enter/editor detour.
    open_notes(m2)
    # Esc cancels the delete confirm: the note must still be there after.
    m2.key("d"); time.sleep(0.4)
    m2.key("esc"); time.sleep(0.3)
    m2.key("esc"); time.sleep(0.3)  # close the app

    to_shell(m2)
    m2.shell("cd NOTES")
    m2.shell("cd NOTES")
    m2.shell("ls")
    m2.shell("cat N0000001.TXT")
    serial = m2.serial()
    if cat_len(serial, 1, "N0000001.TXT") is None:
        fail("Esc at the delete confirm still deleted the note -- delete must ask first and honor a no")
    else:
        print("PASS: Esc at the delete confirm leaves the note alone")

    to_gui(m2)  # same reasoning: dock click -> notes list directly
    open_notes(m2)
    m2.key("d"); time.sleep(0.4)
    m2.type_text("yes")
    m2.key("ret"); time.sleep(0.5)
    m2.key("esc"); time.sleep(0.3)

    to_shell(m2)
    m2.shell("cd NOTES")
    m2.shell("cd NOTES")
    m2.shell("ls")
    m2.shell("cat N0000001.TXT")
    serial = m2.serial()
    if cat_len(serial, 2, "N0000001.TXT") is not None:
        fail("typing yes at the delete confirm did not actually delete the note file")
    else:
        print("PASS: typing yes at the delete confirm really removes the note file")
finally:
    m2.quit()

if fails:
    print(f"\n{len(fails)} FAILURE(S)")
    sys.exit(1)
print("\nPASS: Notes folders -- migration intact, new notes land in the current folder, survive reboot, delete asks first")
