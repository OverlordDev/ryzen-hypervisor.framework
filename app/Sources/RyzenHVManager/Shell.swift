import Foundation

struct CommandResult {
    let status: Int32
    let output: String
}

/// Collects the output of a process and hands it on line by line.
final class LineBuffer {
    private let lock = NSLock()
    private var pending = ""
    private var everything = ""
    private let onLine: (String) -> Void

    init(_ onLine: @escaping (String) -> Void) { self.onLine = onLine }

    func append(_ data: Data) {
        let text = String(decoding: data, as: UTF8.self)
        lock.lock()
        everything += text
        pending += text
        var lines: [String] = []
        while let nl = pending.firstIndex(of: "\n") {
            lines.append(String(pending[..<nl]))
            pending.removeSubrange(...nl)
        }
        lock.unlock()
        lines.forEach(onLine)
    }

    func flush() {
        lock.lock()
        let rest = pending
        pending = ""
        lock.unlock()
        if !rest.isEmpty { onLine(rest) }
    }

    var all: String {
        lock.lock(); defer { lock.unlock() }
        return everything
    }
}

enum Shell {
    static func quote(_ s: String) -> String {
        "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    /// Runs a program and streams its output.
    static func run(_ launch: String, _ args: [String] = [], env: [String: String] = [:],
                    onLine: @escaping (String) -> Void = { _ in }) async -> CommandResult {
        await withCheckedContinuation { (cont: CheckedContinuation<CommandResult, Never>) in
            let process = Process()
            process.executableURL = URL(fileURLWithPath: launch)
            process.arguments = args
            var environment = ProcessInfo.processInfo.environment
            for (k, v) in env { environment[k] = v }
            process.environment = environment

            let pipe = Pipe()
            process.standardOutput = pipe
            process.standardError = pipe
            let buffer = LineBuffer(onLine)
            pipe.fileHandleForReading.readabilityHandler = { handle in
                let data = handle.availableData
                if !data.isEmpty { buffer.append(data) }
            }
            process.terminationHandler = { proc in
                pipe.fileHandleForReading.readabilityHandler = nil
                let rest = pipe.fileHandleForReading.readDataToEndOfFile()
                if !rest.isEmpty { buffer.append(rest) }
                buffer.flush()
                cont.resume(returning: CommandResult(status: proc.terminationStatus, output: buffer.all))
            }
            do {
                try process.run()
            } catch {
                pipe.fileHandleForReading.readabilityHandler = nil
                cont.resume(returning: CommandResult(status: 127, output: "\(error)"))
            }
        }
    }

    /// Runs a shell command line as root: macOS asks for the password in its own dialog.
    /// The output arrives when the command finishes.
    static func admin(_ command: String, onLine: @escaping (String) -> Void = { _ in }) async -> CommandResult {
        let escaped = (command + " 2>&1")
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        let script = "do shell script \"\(escaped)\" with administrator privileges"
        let result = await run("/usr/bin/osascript", ["-e", script])
        if result.status != 0 && result.output.contains("-128") {
            let text = "Cancelled by the user."
            onLine(text)
            return CommandResult(status: result.status, output: text)
        }
        // `do shell script` returns its output with carriage returns as line ends.
        let text = result.output.replacingOccurrences(of: "\r", with: "\n")
        text.split(separator: "\n", omittingEmptySubsequences: true).forEach { onLine(String($0)) }
        return CommandResult(status: result.status, output: text)
    }
}
