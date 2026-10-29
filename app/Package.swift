// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "RyzenHVManager",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "RyzenHVManager",
            path: "Sources/RyzenHVManager",
            swiftSettings: [.swiftLanguageMode(.v5)]
        )
    ]
)
