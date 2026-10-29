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
                    text: model.parallelsEnabled ? "Running through RyzenHV" : "Not enabled")
                Row(level: model.kextLoaded ? .ok : .fail,
                    text: model.kextLoaded ? "The kext answers" : "The kext does not answer: install it first")
            }
            Section(title: "Actions") {
                HStack {
                    Button("Enable (your password is needed)") { model.enableParallels() }
                        .buttonStyle(.borderedProminent)
                        .disabled(model.busy != nil || !model.parallelsInstalled || !model.kextLoaded)
                    Button("Turn off 3D in all VMs") { model.fix3D() }
                        .disabled(model.busy != nil || !model.parallelsInstalled)
                    Button("Undo") { model.undoParallels() }
                        .disabled(model.busy != nil || !model.parallelsEnabled)
                }
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
