import SwiftUI

struct KextView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        Page_(title: "Install the kext into OpenCore") {
            Section(title: "1. The release") {
                if let v = model.bundleVersion {
                    Row(level: .ok, text: "Kext version \(v) is downloaded")
                } else {
                    Text("The kext is part of the GitHub release.")
                    Button("Download from GitHub") { model.downloadLatest() }
                        .disabled(model.latest == nil || model.busy != nil)
                    if model.latest == nil { Text(model.latestError ?? "…").foregroundStyle(.secondary) }
                }
            }

            Section(title: "2. A way back") {
                Text("A kext that panics at boot stops the Mac from starting. Keep a working EFI without it, for example on a USB stick.")
                    .fixedSize(horizontal: false, vertical: true)
                Toggle("I have a working EFI without this kext", isOn: $model.haveBackupEFI)
            }

            Section(title: "3. The EFI partition") {
                HStack {
                    Button("Find EFI") { model.findEFI() }.disabled(model.busy != nil)
                    Text("If the partition is not mounted, your password is needed.")
                        .foregroundStyle(.secondary)
                }
                if !model.efiVolumes.isEmpty {
                    Picker("The EFI you boot from", selection: $model.selectedEFI) {
                        ForEach(model.efiVolumes) { Text($0.title).tag($0.path) }
                    }
                    .pickerStyle(.radioGroup)
                }
            }

            Section(title: "4. Install") {
                Text("What will happen:")
                VStack(alignment: .leading, spacing: 4) {
                    Text("• " + "RyzenHV.kext is copied to EFI/OC/Kexts (an older copy stays as .bak)")
                    Text("• " + "config.plist: a Kernel → Add entry is added (a backup is kept next to it)")
                    Text("• " + "boot-args: rhv_enable_svm=1 rhv_allow_users=1 amfi_get_out_of_my_way=0x1 (yours stay)")
                }
                .font(.callout)
                Text("amfi_get_out_of_my_way weakens system protection and rhv_allow_users gives every local user access to the hypervisor. Use it on a personal machine only.")
                    .font(.callout).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
                Button("Install (your password is needed)") { model.installKext() }
                    .buttonStyle(.borderedProminent)
                    .disabled(model.busy != nil || model.bundleVersion == nil || model.selectedEFI.isEmpty || !model.haveBackupEFI)
                if model.kextDone {
                    Row(level: .ok, text: "Done. Restart the Mac, boot from this EFI, then open Status.")
                }
            }
        }
    }
}
