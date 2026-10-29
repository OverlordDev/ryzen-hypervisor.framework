import SwiftUI

@main
struct RyzenHVManagerApp: App {
    @StateObject private var model = Model()

    var body: some Scene {
        WindowGroup("RyzenHV") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 760, minHeight: 520)
                .onAppear {
                    model.refresh()
                    model.checkLatest()
                }
        }
    }
}
