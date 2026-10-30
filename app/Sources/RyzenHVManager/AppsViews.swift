import SwiftUI

struct ParallelsView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        Page_(title: "Parallels Desktop") {
            Section(title: "State") {
                if !model.parallelsInstalled {
                    Row(level: .info, text: "Parallels Desktop is not installed")
                } else if model.parallelsSupported {
                    Row(level: .ok, text: "Build \(model.parallelsBuild ?? "?") is supported")
                } else {
                    Row(level: .warn, text: "Build \(model.parallelsBuild ?? "?"): only \(model.parallelsSupportedBuild) is tested. The patches are found by their bytes and may not fit.")
                }
                Row(level: model.parallelsEnabled ? .ok : .info,
                    text: model.parallelsEnabled ? (model.parallelsNative ? "Running through RyzenHV (native mode)" : "Running through RyzenHV (shim)") : "Not enabled")
                Row(level: model.nativeMode ? (model.hvSupport == "1" ? .ok : .warn) : .info,
                    text: model.nativeMode ? "Native mode: kern.hv_support is \(model.hvSupport.isEmpty ? "?" : model.hvSupport)" : "The kext is not in native mode (boot-arg rhv_hv=1)")
                Row(level: model.kextLoaded ? .ok : .fail,
                    text: model.kextLoaded ? "The kext answers" : "The kext does not answer: install it first")
            }
            Section(title: "Actions") {
                HStack {
                    Button("Enable (your password is needed)") { model.enableParallels() }
                        .buttonStyle(.borderedProminent)
                        .disabled(model.busy != nil || !model.parallelsInstalled || !model.kextLoaded)
                    Button("Enable, native mode (experimental)") { model.enableParallelsNative() }
                        .disabled(model.busy != nil || !model.parallelsInstalled || !model.nativeMode || model.hvSupport != "1" || !model.parallelsSupported)
                    Button("Turn off 3D in all VMs") { model.fix3D() }
                        .disabled(model.busy != nil || !model.parallelsInstalled)
                    Button("Undo") { model.undoParallels() }
                        .disabled(model.busy != nil || !model.parallelsEnabled)
                }
                Text("Native mode: Parallels uses Apple's own Hypervisor.framework and the kext; its CPU-vendor check is patched and its keyboard armed by a small stand-in library. Tested with Windows 10 only; the keyboard works (the stand-in library arms it). Needs the kext in native mode and kern.hv_support = 1 (see Automatic start).")
                    .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                Text("3D acceleration crashes in the Metal driver and Parallels turns it on in every new VM: press “Turn off 3D” after creating a VM, while it is stopped. Do not kill the VM during the Windows installation. After a Parallels update press Enable again.")
                    .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

struct DockerView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        Page_(title: "Docker Desktop") {
            Section(title: "State") {
                if !model.dockerInstalled {
                    Row(level: .info, text: "Docker Desktop is not installed")
                } else if model.dockerSupported {
                    Row(level: .ok, text: "Docker Desktop \(Apps.dockerVersion) (build \(Apps.dockerBuild))")
                } else {
                    Row(level: .warn, text: "Build \(model.dockerBuild ?? "?"): \(Apps.dockerVersion) (\(Apps.dockerBuild)) is needed. The button below downloads it.")
                }
                Row(level: model.dockerEnabled ? .ok : .info,
                    text: model.dockerEnabled ? "Running through RyzenHV" : "Not enabled")
                Row(level: model.kextLoaded ? .ok : .fail,
                    text: model.kextLoaded ? "The kext answers" : "The kext does not answer: install it first")
            }
            Section(title: "Actions") {
                HStack {
                    Button("Download Docker Desktop \(Apps.dockerVersion)") { model.getDocker() }
                        .disabled(model.busy != nil || model.bundleVersion == nil)
                    Button("Enable") { model.enableDocker() }
                        .buttonStyle(.borderedProminent)
                        .disabled(model.busy != nil || !model.dockerSupported || !model.kextLoaded)
                    Button("Undo") { model.undoDocker() }
                        .disabled(model.busy != nil || !model.dockerEnabled)
                }
                Text("Quit Docker Desktop before any action, then start it as usual. Turn automatic updates off in Settings → Software updates; after an update press Enable again.")
                    .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

struct LogView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Log").font(.title2.bold())
                Spacer()
                Button("Copy") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(model.log.joined(separator: "\n"), forType: .string)
                }
                Button("Clear") { model.log.removeAll() }
            }
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 2) {
                        ForEach(Array(model.log.enumerated()), id: \.offset) { i, line in
                            Text(line).font(.system(.callout, design: .monospaced)).textSelection(.enabled).id(i)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .onChange(of: model.log.count) { _ in
                    if let last = model.log.indices.last { proxy.scrollTo(last, anchor: .bottom) }
                }
            }
            .padding(8)
            .background(RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .textBackgroundColor)))
        }
        .padding(20)
    }
}


struct AutostartView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        Page_(title: "Automatic start") {
            Section(title: "State") {
                Row(level: model.kextLoaded ? .ok : .fail,
                    text: model.kextLoaded ? "The kext answers" : "The kext does not answer: install it first")
                Row(level: model.nativeMode ? .ok : .info,
                    text: model.nativeMode ? "Native mode is on (the kext is the hypervisor provider)" : "The kext is not in native mode (boot-arg rhv_hv=1): nothing to turn on")
                Row(level: model.hvSupport == "1" ? .ok : .warn,
                    text: "kern.hv_support is \(model.hvSupport.isEmpty ? "unknown" : model.hvSupport)")
                Row(level: model.autostartInstalled ? .ok : .info,
                    text: model.autostartInstalled ? "The boot service is installed" : "The boot service is not installed")
            }
            Section(title: "Actions") {
                HStack {
                    Button("Install (your password is needed)") { model.installAutostart() }
                        .buttonStyle(.borderedProminent)
                        .disabled(model.busy != nil || !model.autostartAvailable || !model.nativeMode || model.autostartInstalled)
                    Button("Turn on now") { model.turnOnHvSupportNow() }
                        .disabled(model.busy != nil || !model.nativeMode || model.hvSupport == "1")
                    Button("Remove") { model.removeAutostart() }
                        .disabled(model.busy != nil || !model.autostartInstalled)
                }
                if !model.autostartAvailable && model.bundleDir != nil {
                    Text("This version of the bundle has no boot service: update it on the Status page.")
                        .font(.callout).foregroundStyle(.orange)
                }
                Text("Parallels (and QEMU) look at kern.hv_support, which is 0 after every restart. The boot service is a small launchd daemon that waits for the kext and turns it on, the same as “Turn on now”. It copies rhv-info to /usr/local/libexec/ryzenhv (owned by root) and writes /Library/LaunchDaemons/com.overlorddev.ryzenhv.hvsysctl.plist; “Remove” deletes both.")
                    .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}
