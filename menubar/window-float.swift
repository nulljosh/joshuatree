// Real always-on-top for the QEMU cocoa window, the gap roadmap.md's own
// "Window-level requests" entry named: no QEMU -display flag for this,
// and AppleScript/System Events exposes no window-level property, only
// position/size/minimize. Finding the window (CGWindowListCopyWindowInfo,
// kCGWindowOwnerPID) is a real public API. Setting its level isn't public
// (no NSWindow.level equivalent reaches across processes), so this uses
// CGSSetWindowLevel, the same private-but-ABI-stable CoreGraphics
// Services symbol every long-running "always on top" utility on macOS
// (Afloat, Contexts, and others) has used for years. It has shipped
// unremoved across every macOS release so far; if Apple ever pulls it,
// the fallback is losing always-on-top, not a crash (CGSSetWindowLevel's
// own return code is checked, never force-unwrapped).
//
// Runs for the whole life of one qemu-system-i386 process, not a system
// daemon: a foreground helper 1:1 with one running kernel session,
// backgrounded by JoshuaTree.app's own launcher and killed the moment
// qemu exits, same lifetime as sync_dotfiles.sh's own one-shot call in
// that script, just longer-lived because the job (re-floating a window
// that gets a new CGWindowID every time native full screen is toggled)
// genuinely needs to persist for the session, not a poll-forever daemon
// installed outside this app's own lifecycle.
import CoreGraphics
import Foundation

typealias CGSConnectionID = UInt32
@_silgen_name("CGSMainConnectionID")
func CGSMainConnectionID() -> CGSConnectionID
@_silgen_name("CGSSetWindowLevel")
func CGSSetWindowLevel(_ cid: CGSConnectionID, _ wid: CGWindowID, _ level: Int32) -> Int32

guard CommandLine.arguments.count > 1, let targetPID = pid_t(CommandLine.arguments[1]) else {
    FileHandle.standardError.write("usage: window-float <qemu-pid>\n".data(using: .utf8)!)
    exit(1)
}

func isAlive(_ pid: pid_t) -> Bool { kill(pid, 0) == 0 }

func currentWindowIDs(forPID pid: pid_t) -> [CGWindowID] {
    let info = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: AnyObject]] ?? []
    return info.compactMap { entry in
        guard (entry[kCGWindowOwnerPID as String] as? pid_t) == pid else { return nil }
        return entry[kCGWindowNumber as String] as? CGWindowID
    }
}

let level = CGWindowLevelForKey(.floatingWindow)
let cid = CGSMainConnectionID()
var floated = Set<CGWindowID>()

// Re-check every second rather than once at launch: toggling native full
// screen (the green button, or Esc back out of it) tears down and
// recreates the window with a new CGWindowID, a real, observed macOS
// behavior, not a hypothetical, so a single apply-once-and-exit would
// silently stop working the first time the user actually used the
// minimize/windowed behavior this same request also asked for.
while isAlive(targetPID) {
    for wid in currentWindowIDs(forPID: targetPID) where !floated.contains(wid) {
        if CGSSetWindowLevel(cid, wid, level) == 0 { floated.insert(wid) }
    }
    Thread.sleep(forTimeInterval: 1.0)
}
