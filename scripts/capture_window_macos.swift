#!/usr/bin/env swift
// SPDX-License-Identifier: MIT
//
// Capture a macOS window by owner PID using CoreGraphics window ids.
// This avoids AppleScript accessibility lookups and works better across
// multi-monitor layouts because CoreGraphics owns the global window geometry.

import CoreGraphics
import Foundation

struct Args {
    var pid: Int32 = -1
    var outPath: String = ""
}

func usage() -> Never {
    fputs("usage: capture_window_macos.swift --pid <pid> --out <png>\n", stderr)
    exit(2)
}

func parseArgs() -> Args {
    var args = Args()
    let argv = CommandLine.arguments
    var i = 1
    while i < argv.count {
        switch argv[i] {
        case "--pid":
            i += 1
            if i >= argv.count { usage() }
            guard let value = Int32(argv[i]) else { usage() }
            args.pid = value
        case "--out":
            i += 1
            if i >= argv.count { usage() }
            args.outPath = argv[i]
        default:
            usage()
        }
        i += 1
    }
    if args.pid <= 0 || args.outPath.isEmpty { usage() }
    return args
}

func intValue(_ dict: [String: Any], _ key: CFString) -> Int? {
    if let n = dict[key as String] as? NSNumber {
        return n.intValue
    }
    return nil
}

func rectValue(_ dict: [String: Any], _ key: CFString) -> CGRect? {
    guard let bounds = dict[key as String] as? NSDictionary else {
        return nil
    }
    var rect = CGRect.zero
    return CGRectMakeWithDictionaryRepresentation(bounds, &rect) ? rect : nil
}

let args = parseArgs()
guard let rawList = CGWindowListCopyWindowInfo([.optionOnScreenOnly,
                                                .excludeDesktopElements],
                                               kCGNullWindowID) as? [[String: Any]] else {
    fputs("[capture-window] CoreGraphics window list unavailable\n", stderr)
    exit(1)
}

var bestWindow: [String: Any]?
var bestArea = 0.0

for window in rawList {
    guard intValue(window, kCGWindowOwnerPID) == Int(args.pid) else { continue }
    let layer = intValue(window, kCGWindowLayer) ?? 0
    guard layer == 0 else { continue }
    guard let rect = rectValue(window, kCGWindowBounds) else { continue }
    guard rect.width >= 32.0 && rect.height >= 32.0 else { continue }
    let alpha = (window[kCGWindowAlpha as String] as? NSNumber)?.doubleValue ?? 1.0
    guard alpha > 0.01 else { continue }
    let area = rect.width * rect.height
    if area > bestArea {
        bestWindow = window
        bestArea = area
    }
}

guard let window = bestWindow,
      let windowIdValue = intValue(window, kCGWindowNumber),
      let rect = rectValue(window, kCGWindowBounds) else {
    fputs("[capture-window] no visible layer-0 window for pid \(args.pid)\n", stderr)
    exit(1)
}

let outURL = URL(fileURLWithPath: args.outPath)
try? FileManager.default.createDirectory(at: outURL.deletingLastPathComponent(),
                                         withIntermediateDirectories: true)

let task = Process()
task.executableURL = URL(fileURLWithPath: "/usr/sbin/screencapture")
task.arguments = ["-x", "-l\(windowIdValue)", args.outPath]
do {
    try task.run()
    task.waitUntilExit()
} catch {
    fputs("[capture-window] failed to launch screencapture: \(error)\n", stderr)
    exit(1)
}

guard task.terminationStatus == 0 else {
    fputs("[capture-window] screencapture failed for window id \(windowIdValue)\n", stderr)
    exit(1)
}

let attrs = try? FileManager.default.attributesOfItem(atPath: args.outPath)
let byteCount = (attrs?[.size] as? NSNumber)?.int64Value ?? 0
guard byteCount > 0 else {
    fputs("[capture-window] screencapture wrote no bytes for \(args.outPath)\n", stderr)
    exit(1)
}

let owner = (window[kCGWindowOwnerName as String] as? String) ?? "unknown"
let title = (window[kCGWindowName as String] as? String) ?? ""
print("[capture-window] pid=\(args.pid) id=\(windowIdValue) owner=\"\(owner)\" title=\"\(title)\"")
print("[capture-window] bounds x=\(Int(rect.origin.x)) y=\(Int(rect.origin.y)) w=\(Int(rect.width)) h=\(Int(rect.height))")
print("[capture-window] wrote \(args.outPath) bytes=\(byteCount)")
