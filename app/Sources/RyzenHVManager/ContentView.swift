import SwiftUI

enum Page: String, CaseIterable, Identifiable {
    case status, kext, autostart, parallels, docker, log
    var id: String { rawValue }
    var title: String {
        switch self {
        case .status: return "Status"
        case .kext: return "Install the kext"
        case .autostart: return "Automatic start"
        case .parallels: return "Parallels Desktop"
        case .docker: return "Docker Desktop"
        case .log: return "Log"
        }
    }
    var icon: String {
        switch self {
        case .status: return "checkmark.shield"
        case .kext: return "cpu"
        case .autostart: return "bolt"
        case .parallels: return "display"
        case .docker: return "shippingbox"
        case .log: return "text.alignleft"
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var model: Model
    @State private var page: Page? = .status

    var body: some View {
        NavigationSplitView {
            List(Page.allCases, selection: $page) { p in
                Label(p.title, systemImage: p.icon).tag(p)
            }
            .navigationSplitViewColumnWidth(min: 180, ideal: 200)
        } detail: {
            VStack(spacing: 0) {
                Group {
                    switch page ?? .status {
                    case .status: StatusView()
                    case .kext: KextView()
                    case .autostart: AutostartView()
                    case .parallels: ParallelsView()
                    case .docker: DockerView()
                    case .log: LogView()
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                if let busy = model.busy { BusyBar(title: busy) }
            }
        }
    }
}

struct BusyBar: View {
    @EnvironmentObject var model: Model
    let title: String

    var body: some View {
        HStack(spacing: 10) {
            if let p = model.progress { ProgressView(value: p).frame(width: 140) } else { ProgressView().controlSize(.small) }
            Text(title).lineLimit(1)
            Spacer()
        }
        .padding(10)
        .background(.bar)
    }
}

/// A titled block of a page.
struct Section<Content: View>: View {
    let title: String
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.headline)
            content
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10).fill(Color(nsColor: .controlBackgroundColor)))
    }
}

struct Page_<Content: View>: View {
    let title: String
    @ViewBuilder var content: Content

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                Text(title).font(.title2.bold())
                content
            }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

struct Row: View {
    let level: Check.Level
    let text: String

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Image(systemName: icon).foregroundStyle(color).frame(width: 18)
            Text(text).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
        }
    }

    private var icon: String {
        switch level {
        case .ok: return "checkmark.circle.fill"
        case .warn: return "exclamationmark.triangle.fill"
        case .fail: return "xmark.octagon.fill"
        case .info: return "minus.circle"
        }
    }
    private var color: Color {
        switch level {
        case .ok: return .green
        case .warn: return .orange
        case .fail: return .red
        case .info: return .secondary
        }
    }
}
