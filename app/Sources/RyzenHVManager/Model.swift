import Foundation
import SwiftUI

@MainActor
final class Model: ObservableObject {
    @Published var log: [String] = []
    @Published var busy: String?
    @Published var progress: Double?

    @Published var checks: [Check] = []
    @Published var bundleVersion: String?
    @Published var kextLoaded = false
    @Published var latest: ReleaseInfo?
    @Published var latestError: String?

    @Published var efiVolumes: [EFIVolume] = []
    @Published var selectedEFI = ""
    @Published var haveBackupEFI = false
    @Published var kextDone = false

    @Published var parallelsWrapper = ""
    @Published var dockerWrapper = ""
    @Published var nativeMode = false          // the kext is the hypervisor provider (booted with rhv_hv=1)
    @Published var hvSupport = ""              // sysctl kern.hv_support
    @Published var autostartStatus = ""        // output of libexec/hv-sysctl-daemon.sh status

    private let user = NSUserName()

    var bundleDir: URL? { Store.current?.dir }
    var rhv: String? { bundleDir?.appendingPathComponent("rhv").path }
    var hasBlockingProblem: Bool { checks.contains { $0.level == .fail } }

    // MARK: log and busy state

    func say(_ line: String) {
        let clean = line.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !clean.isEmpty else { return }
        log.append(clean)
        if log.count > 3000 { log.removeFirst(500) }
    }

    private func line(_ unused: String = "") -> (String) -> Void {
        { [weak self] l in Task { @MainActor in self?.say(l) } }
    }

    /// Runs one action at a time, showing its title while it runs.
    private func task(_ title: String, _ work: @escaping () async -> Void) {
        guard busy == nil else { return }
        busy = title
        say("— " + title)
        Task {
            await work()
            progress = nil
            busy = nil
            await refreshNow()
        }
    }

    // MARK: status

    func refresh() {
        guard busy == nil else { return }
        Task { await refreshNow() }
    }

    func refreshNow() async {
        bundleVersion = Store.current?.version
        guard let dir = bundleDir else {
            checks = []
            kextLoaded = false
            parallelsWrapper = ""
            dockerWrapper = ""
            nativeMode = false
            hvSupport = ""
            autostartStatus = ""
            return
        }
        let doctor = await Shell.run(dir.appendingPathComponent("rhv").path, ["doctor"])
        checks = Check.parse(doctor.output)
        let info = await Shell.run(dir.appendingPathComponent("bin/rhv-info").path)
        kextLoaded = info.status == 0 && info.output.contains("RyzenHV kext")
        let hv = await Shell.run(dir.appendingPathComponent("bin/rhv-info").path, ["hv"])
        nativeMode = hv.output.contains("front end: registered")
        hvSupport = (await Shell.run("/usr/sbin/sysctl", ["-n", "kern.hv_support"])).output.trimmingCharacters(in: .whitespacesAndNewlines)
        autostartStatus = await Shell.run(dir.appendingPathComponent("libexec/hv-sysctl-daemon.sh").path, ["status"]).output
        parallelsWrapper = await Shell.run(dir.appendingPathComponent("libexec/parallels-shim.sh").path, ["status"]).output
        dockerWrapper = await Shell.run(dir.appendingPathComponent("libexec/docker-desktop-shim.sh").path, ["status"]).output
    }

    func checkLatest() {
        Task {
            do {
                latest = try await GitHub.latest()
                latestError = nil
            } catch {
                latest = nil
                latestError = error.localizedDescription
            }
        }
    }

    var updateAvailable: Bool {
        guard let latest else { return false }
        guard let have = bundleVersion else { return true }
        return latest.version.compare(have, options: .numeric) == .orderedDescending
    }

    // MARK: release download

    func downloadLatest() {
        guard let release = latest else { return }
        task("Downloading \(release.tag) from GitHub") { [self] in
            do {
                let archive = Store.root.appendingPathComponent("dl/" + release.tarball.lastPathComponent)
                say(release.tarball.absoluteString)
                progress = 0
                _ = try await Downloader(destination: archive) { p in
                    Task { @MainActor in self.progress = p }
                }.download(release.tarball)
                progress = nil

                if let sumURL = release.checksum {
                    let (data, _) = try await URLSession.shared.data(from: sumURL)
                    let expected = String(decoding: data, as: UTF8.self).split(separator: " ").first.map(String.init) ?? ""
                    let actual = await Shell.run("/usr/bin/shasum", ["-a", "256", archive.path]).output
                        .split(separator: " ").first.map(String.init) ?? ""
                    guard !expected.isEmpty, expected == actual else {
                        throw AppError("Checksum mismatch, the archive is not used")
                    }
                    say("SHA-256 checksum matches")
                } else {
                    say("The release has no checksum file, verification skipped")
                }

                let extract = await Shell.run("/usr/bin/tar", ["-xzf", archive.path, "-C", Store.root.path], onLine: line(""))
                guard extract.status == 0 else { throw AppError("Could not unpack the archive") }
                let folder = Store.root.appendingPathComponent("ryzenhv-" + release.version)
                _ = await Shell.run("/usr/bin/xattr", ["-dr", "com.apple.quarantine", folder.path])
                try? FileManager.default.removeItem(at: archive)
                say("Done: version \(release.version)")
            } catch {
                say("Error: " + error.localizedDescription)
            }
        }
    }

    // MARK: kext into the OpenCore EFI

    private func scanEFI() -> [EFIVolume] {
        let fm = FileManager.default
        let names = (try? fm.contentsOfDirectory(atPath: "/Volumes")) ?? []
        return names.compactMap { name in
            let config = "/Volumes/\(name)/EFI/OC/config.plist"
            guard fm.fileExists(atPath: config) else { return nil }
            let date = (try? fm.attributesOfItem(atPath: config))?[.modificationDate] as? Date
            return EFIVolume(path: "/Volumes/" + name, configDate: date)
        }
    }

    func findEFI() {
        task("Looking for the OpenCore EFI partition") { [self] in
            var found = scanEFI()
            if found.isEmpty {
                say("Mounting EFI partitions (your password is needed)…")
                let script = "for s in $(/usr/sbin/diskutil list | /usr/bin/awk '$2 == \"EFI\" { print $NF }'); do /sbin/mount | /usr/bin/grep -q \"^/dev/$s \" || /usr/sbin/diskutil mount /dev/$s; done"
                _ = await Shell.admin(script, onLine: line(""))
                found = scanEFI()
            }
            efiVolumes = found
            if !found.contains(where: { $0.path == selectedEFI }) { selectedEFI = found.first?.path ?? "" }
            say(found.isEmpty
                ? "No OpenCore EFI found. Mount it yourself and press Find again."
                : "Found: \(found.count)")
        }
    }

    func installKext() {
        guard let rhv, !selectedEFI.isEmpty, haveBackupEFI else { return }
        let efi = selectedEFI
        task("Installing the kext into \(efi)") { [self] in
            let result = await Shell.admin("RHV_YES=1 \(Shell.quote(rhv)) kext \(Shell.quote(efi))", onLine: line(""))
            kextDone = result.status == 0
            say(kextDone
                ? "The kext is installed. Restart the Mac, then open Status."
                : "Installation failed; the script restored the original config.plist.")
        }
    }

    // MARK: Parallels

    var parallelsInstalled: Bool { FileManager.default.fileExists(atPath: Apps.parallels) }
    var parallelsBuild: String? { Apps.build(of: Apps.parallels) }
    var parallelsSupportedBuild: String { bundleDir.flatMap { Store.value("parallels_build", in: $0) } ?? "57613" }
    var parallelsSupported: Bool { parallelsBuild == parallelsSupportedBuild }
    var parallelsEnabled: Bool { parallelsWrapper.contains("wrapper installed") }
    var parallelsNative: Bool { parallelsWrapper.contains("mode: native") }

    func enableParallels() {
        guard let dir = bundleDir else { return }
        task("Enabling Parallels Desktop") { [self] in
            let script = dir.appendingPathComponent("libexec/parallels-shim.sh").path
            say("1/3 Preparing the private copy of Parallels with the shim…")
            let prepare = await Shell.run(script, ["prepare"], onLine: line(""))
            guard prepare.status == 0 else { say("Preparation failed."); return }
            say("2/3 Installing the wrapper in place of prl_vm_app (your password is needed)…")
            let install = await Shell.admin("SUDO_USER=\(Shell.quote(user)) \(Shell.quote(script)) install", onLine: line(""))
            guard install.status == 0 else { say("Installing the wrapper failed."); return }
            say("3/3 Turning 3D acceleration off in existing VMs…")
            await turnOff3D()
            say("Parallels is ready. For each new VM press “Turn off 3D”.")
        }
    }

    /// Native mode (docs/NATIVE.md): Parallels runs on Apple's own Hypervisor.framework and the kext; its CPU-vendor check is patched and its keyboard armed by a small stand-in library.
    func enableParallelsNative() {
        guard let dir = bundleDir else { return }
        task("Enabling Parallels Desktop (native mode)") { [self] in
            guard nativeMode else { say("The kext is not in native mode: install it with “Native mode” on the Install page, restart, then try again."); return }
            guard hvSupport == "1" else { say("kern.hv_support is \(hvSupport.isEmpty ? "unknown" : hvSupport): Parallels would use its own engine. Open “Automatic start” first."); return }
            let script = dir.appendingPathComponent("libexec/parallels-shim.sh").path
            say("1/3 Preparing the private copy of Parallels (native mode)…")
            let prepare = await Shell.run(script, ["prepare"], env: ["PATCHONLY": "1"], onLine: line(""))
            guard prepare.status == 0 else { say("Preparation failed."); return }
            say("2/3 Installing the wrapper in place of prl_vm_app (your password is needed)…")
            let install = await Shell.admin("SUDO_USER=\(Shell.quote(user)) \(Shell.quote(script)) install", onLine: line(""))
            guard install.status == 0 else { say("Installing the wrapper failed."); return }
            say("3/3 Turning 3D acceleration off in existing VMs…")
            await turnOff3D()
            say("Parallels is ready (native mode). Set the VM's hypervisor type to Apple.")
        }
    }

    // MARK: automatic start of kern.hv_support

    var autostartAvailable: Bool {
        guard let dir = bundleDir else { return false }
        return FileManager.default.isExecutableFile(atPath: dir.appendingPathComponent("libexec/hv-sysctl-daemon.sh").path)
    }
    var autostartInstalled: Bool { autostartStatus.contains("daemon file: /Library") }

    func installAutostart() {
        guard let rhv else { return }
        task("Installing the boot service") { [self] in
            let result = await Shell.admin("\(Shell.quote(rhv)) autostart install", onLine: line(""))
            say(result.status == 0 ? "Done. kern.hv_support is turned on at every boot." : "The boot service was not installed.")
        }
    }

    func removeAutostart() {
        guard let rhv else { return }
        task("Removing the boot service") { [self] in
            _ = await Shell.admin("\(Shell.quote(rhv)) autostart remove", onLine: line(""))
        }
    }

    func turnOnHvSupportNow() {
        guard let dir = bundleDir else { return }
        task("Turning kern.hv_support on") { [self] in
            _ = await Shell.run(dir.appendingPathComponent("bin/rhv-info").path, ["hv", "sysctl", "1"], onLine: line(""))
        }
    }

    func undoParallels() {
        guard let dir = bundleDir else { return }
        task("Restoring Parallels' own files") { [self] in
            let script = dir.appendingPathComponent("libexec/parallels-shim.sh").path
            _ = await Shell.admin("SUDO_USER=\(Shell.quote(user)) \(Shell.quote(script)) uninstall", onLine: line(""))
        }
    }

    func fix3D() {
        task("Turning 3D off in Parallels VMs") { [self] in await turnOff3D() }
    }

    /// Parallels switches 3D acceleration on in every new VM; here it crashes the VM process. Only stopped VMs can be changed.
    private func turnOff3D() async {
        let prlctl = "/usr/local/bin/prlctl"
        guard FileManager.default.isExecutableFile(atPath: prlctl) else { say("prlctl: " + "not found"); return }
        let list = await Shell.run(prlctl, ["list", "-a", "-o", "uuid,status"])
        var changed = 0
        for row in list.output.split(separator: "\n").dropFirst() {
            let parts = row.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
            guard parts.count >= 2 else { continue }
            guard parts[1] == "stopped" else { say("\(parts[0]): \(parts[1]) — " + "skipped, stop the VM first"); continue }
            let set = await Shell.run(prlctl, ["set", parts[0], "--3d-accelerate", "off", "--high-resolution", "off",
                                               "--high-resolution-in-guest", "off", "--vertical-sync", "off"])
            if set.status == 0 { changed += 1; say("\(parts[0]): " + "3D is off") } else { say(set.output) }
        }
        if changed == 0 { say("No stopped VMs to change.") }
    }

    // MARK: Docker Desktop

    var dockerInstalled: Bool { FileManager.default.fileExists(atPath: Apps.docker) }
    var dockerBuild: String? { Apps.build(of: Apps.docker) }
    var dockerSupported: Bool { dockerBuild == Apps.dockerBuild }
    var dockerEnabled: Bool { dockerWrapper.contains("installed (HyperKit and backend wrapped)") }

    private func dockerRunning() async -> Bool {
        await Shell.run("/usr/bin/pgrep", ["-f", Apps.docker + "/Contents/MacOS/com.docker.backend"]).status == 0
    }

    func getDocker() {
        guard let rhv else { return }
        task("Downloading Docker Desktop \(Apps.dockerVersion)") { [self] in
            if await dockerRunning() { say("Quit Docker Desktop first (whale in the menu bar → Quit)."); return }
            _ = await Shell.run(rhv, ["docker-desktop", "get"], env: ["RHV_YES": "1"], onLine: line(""))
        }
    }

    func enableDocker() {
        guard let rhv else { return }
        task("Enabling Docker Desktop") { [self] in
            if await dockerRunning() { say("Quit Docker Desktop first (whale in the menu bar → Quit)."); return }
            let result = await Shell.run(rhv, ["docker-desktop", "setup"], env: ["RHV_YES": "1"], onLine: line(""))
            if result.status == 0 { say("Done. Start Docker as usual.") }
        }
    }

    func undoDocker() {
        guard let rhv else { return }
        task("Restoring Docker Desktop's own files") { [self] in
            if await dockerRunning() { say("Quit Docker Desktop first."); return }
            _ = await Shell.run(rhv, ["docker-desktop", "undo"], env: ["RHV_YES": "1"], onLine: line(""))
        }
    }
}
