import SwiftUI

struct Progress {
    let done: Int
    let total: Int
    let commitSubject: String
    let commitAge: String
}

@MainActor
final class ProgressPoller: ObservableObject {
    @Published var progress: Progress?

    private let repoPath = NSString(
        string: "~/Documents/Code/joshuatree"
    ).expandingTildeInPath

    init() {
        poll()
        Timer.scheduledTimer(withTimeInterval: 15, repeats: true) { _ in
            Task { @MainActor in self.poll() }
        }
    }

    func poll() {
        let roadmapPath = "\(repoPath)/roadmap.md"
        guard let text = try? String(contentsOfFile: roadmapPath, encoding: .utf8) else { return }

        var done = 0, total = 0
        for line in text.split(separator: "\n") {
            if line.hasPrefix("- [x]") { done += 1; total += 1 }
            else if line.hasPrefix("- [ ]") { total += 1 }
        }

        let (subject, age) = latestCommit()
        progress = Progress(done: done, total: total, commitSubject: subject, commitAge: age)
    }

    private func latestCommit() -> (String, String) {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/git")
        task.arguments = ["-C", repoPath, "log", "-1", "--format=%s|||%cr"]
        let pipe = Pipe()
        task.standardOutput = pipe
        try? task.run()
        task.waitUntilExit()
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        let out = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let parts = out.components(separatedBy: "|||")
        return parts.count == 2 ? (parts[0], parts[1]) : ("no commits yet", "")
    }
}

@main
struct JoshuaTreeMonitorApp: App {
    @StateObject private var poller = ProgressPoller()

    var body: some Scene {
        MenuBarExtra {
            VStack(alignment: .leading, spacing: 10) {
                if let p = poller.progress {
                    HStack(spacing: 4) {
                        Text("Joshua Tree")
                            .font(.system(size: 12, weight: .semibold))
                        Spacer()
                        Text("\(p.done)/\(p.total) shipped")
                            .font(.system(size: 11))
                            .foregroundStyle(.secondary)
                    }
                    Divider()
                    Text("Latest")
                        .font(.system(size: 10, weight: .semibold))
                        .foregroundStyle(.secondary)
                    Text(p.commitSubject)
                        .font(.system(size: 11))
                        .lineLimit(3)
                        .fixedSize(horizontal: false, vertical: true)
                    Text(p.commitAge)
                        .font(.system(size: 10))
                        .foregroundStyle(.secondary)
                } else {
                    Text("No progress data yet")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                }

                Divider()
                Button("Open repo") {
                    NSWorkspace.shared.open(URL(fileURLWithPath: NSString(string: "~/Documents/Code/joshuatree").expandingTildeInPath))
                }
                .font(.system(size: 12))
                Button("Quit monitor") { NSApplication.shared.terminate(nil) }
                    .font(.system(size: 12))
            }
            .padding(12)
            .frame(width: 240)
        } label: {
            Text(labelText)
                .font(.system(size: 12, design: .monospaced))
        }
        .menuBarExtraStyle(.window)
    }

    private var labelText: String {
        guard let p = poller.progress else { return "jt …" }
        return "jt \(p.done)/\(p.total)"
    }
}
