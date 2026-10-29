import Foundation

struct ReleaseInfo {
    let tag: String
    let tarball: URL
    let checksum: URL?
    var version: String { tag.hasPrefix("v") ? String(tag.dropFirst()) : tag }
}

enum GitHub {
    static let repo = "OverlordDev/ryzen-hypervisor.framework"

    static func latest() async throws -> ReleaseInfo {
        var request = URLRequest(url: URL(string: "https://api.github.com/repos/\(repo)/releases/latest")!)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue("RyzenHVManager", forHTTPHeaderField: "User-Agent")
        let (data, response) = try await URLSession.shared.data(for: request)
        guard (response as? HTTPURLResponse)?.statusCode == 200,
              let json = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let tag = json["tag_name"] as? String,
              let assets = json["assets"] as? [[String: Any]] else {
            throw AppError("GitHub did not return the release")
        }
        func asset(_ suffix: String) -> URL? {
            assets.first { ($0["name"] as? String)?.hasSuffix(suffix) == true }
                .flatMap { $0["browser_download_url"] as? String }
                .flatMap(URL.init(string:))
        }
        guard let tar = asset("-macos-x86_64.tar.gz") else {
            throw AppError("The release has no macOS x86_64 archive")
        }
        return ReleaseInfo(tag: tag, tarball: tar, checksum: asset("-macos-x86_64.tar.gz.sha256"))
    }
}

struct AppError: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

/// Downloads a file and reports the progress (0...1).
final class Downloader: NSObject, URLSessionDownloadDelegate {
    private var continuation: CheckedContinuation<URL, Error>?
    private let progress: (Double) -> Void
    private let destination: URL

    init(destination: URL, progress: @escaping (Double) -> Void) {
        self.destination = destination
        self.progress = progress
    }

    func download(_ url: URL) async throws -> URL {
        try await withCheckedThrowingContinuation { cont in
            continuation = cont
            let session = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
            session.downloadTask(with: url).resume()
        }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData _: Int64,
                    totalBytesWritten written: Int64, totalBytesExpectedToWrite total: Int64) {
        if total > 0 { progress(Double(written) / Double(total)) }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        do {
            let fm = FileManager.default
            try fm.createDirectory(at: destination.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? fm.removeItem(at: destination)
            try fm.moveItem(at: location, to: destination)
            continuation?.resume(returning: destination)
        } catch {
            continuation?.resume(throwing: error)
        }
        continuation = nil
        session.finishTasksAndInvalidate()
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error, let cont = continuation {
            cont.resume(throwing: error)
            continuation = nil
        }
        session.finishTasksAndInvalidate()
    }
}

/// The downloaded release bundles (kext, shim, scripts, `rhv`) live here.
enum Store {
    static var root: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/RyzenHV")
    }

    /// Bundles on disk, newest first. RHV_BUNDLE (a folder with `rhv`) overrides the choice, for development.
    static func bundles() -> [(version: String, dir: URL)] {
        if let dev = ProcessInfo.processInfo.environment["RHV_BUNDLE"], !dev.isEmpty {
            let dir = URL(fileURLWithPath: dev)
            return [(version(of: dir) ?? "dev", dir)]
        }
        let names = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
        return names.filter { $0.hasPrefix("ryzenhv-") }
            .map { root.appendingPathComponent($0) }
            .filter { FileManager.default.isExecutableFile(atPath: $0.appendingPathComponent("rhv").path) }
            .map { (version(of: $0) ?? String($0.lastPathComponent.dropFirst(8)), $0) }
            .sorted { $0.version.compare($1.version, options: .numeric) == .orderedDescending }
    }

    static var current: (version: String, dir: URL)? { bundles().first }

    /// A value from the bundle's VERSION file (version=, abi=, parallels_build=).
    static func value(_ key: String, in dir: URL) -> String? {
        guard let text = try? String(contentsOf: dir.appendingPathComponent("VERSION"), encoding: .utf8) else { return nil }
        for line in text.split(separator: "\n") where line.hasPrefix(key + "=") {
            return String(line.dropFirst(key.count + 1))
        }
        return nil
    }

    static func version(of dir: URL) -> String? { value("version", in: dir) }
}

/// One line of `rhv doctor`.
struct Check: Identifiable {
    enum Level { case ok, warn, fail, info }
    let id = UUID()
    let level: Level
    let text: String

    static func parse(_ output: String) -> [Check] {
        output.split(separator: "\n").compactMap { raw in
            let line = raw.trimmingCharacters(in: .whitespaces)
            let table: [(String, Level)] = [("[ok]", .ok), ("[warn]", .warn), ("[FAIL]", .fail), ("[--]", .info)]
            for (tag, level) in table where line.hasPrefix(tag) {
                return Check(level: level, text: line.dropFirst(tag.count).trimmingCharacters(in: .whitespaces))
            }
            return nil
        }
    }
}

/// An OpenCore EFI partition that is mounted.
struct EFIVolume: Identifiable, Hashable {
    let path: String
    let configDate: Date?
    var id: String { path }
    var title: String {
        let date = configDate.map { " — config.plist: " + $0.formatted(date: .abbreviated, time: .shortened) } ?? ""
        return path + date
    }
}

/// Facts about the other applications.
enum Apps {
    static func build(of path: String) -> String? {
        Bundle(path: path)?.infoDictionary?["CFBundleVersion"] as? String
    }
    static func shortVersion(of path: String) -> String? {
        Bundle(path: path)?.infoDictionary?["CFBundleShortVersionString"] as? String
    }
    static let parallels = "/Applications/Parallels Desktop.app"
    static let docker = "/Applications/Docker.app"
    static let dockerBuild = "244584"
    static let dockerVersion = "4.97.0"
}
