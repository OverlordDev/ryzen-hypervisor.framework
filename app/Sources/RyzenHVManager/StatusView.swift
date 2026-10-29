import SwiftUI

struct StatusView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        Page_(title: "Status") {
            Section(title: "RyzenHV release") {
                if let v = model.bundleVersion {
                    Row(level: .ok, text: "Version \(v) is downloaded")
                } else {
                    Row(level: .warn, text: "No release downloaded yet. It holds the kext, the shim and the scripts.")
                }
                if let latest = model.latest {
                    Row(level: model.updateAvailable ? .warn : .ok,
                        text: model.updateAvailable
                            ? "\(latest.tag) is available on GitHub"
                            : "This is the latest version (\(latest.tag))")
                } else if let err = model.latestError {
                    Row(level: .info, text: "GitHub is unreachable: " + err)
                }
                HStack {
                    Button(model.bundleVersion == nil ? "Download from GitHub"
                                                      : "Update from GitHub") { model.downloadLatest() }
                        .disabled(model.latest == nil || model.busy != nil || (model.bundleVersion != nil && !model.updateAvailable))
                    Button("Check again") { model.checkLatest(); model.refresh() }
                        .disabled(model.busy != nil)
                }
            }

            Section(title: "This Mac") {
                if model.checks.isEmpty {
                    Text(model.bundleVersion == nil
                         ? "Download the release to check the system."
                         : "Checking…").foregroundStyle(.secondary)
                }
                ForEach(model.checks) { Row(level: $0.level, text: $0.text) }
                if model.bundleVersion != nil {
                    Button("Check") { model.refresh() }.disabled(model.busy != nil)
                }
            }

            if model.bundleVersion != nil && !model.kextLoaded {
                Section(title: "What next") {
                    Text("The kext does not answer. Open “Install the kext”, follow the steps, then restart the Mac.")
                }
            }
        }
    }
}
