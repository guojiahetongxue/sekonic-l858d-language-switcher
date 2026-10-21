import AppKit

let output = CommandLine.arguments[1]
let sizes = [(16,1),(16,2),(32,1),(32,2),(128,1),(128,2),(256,1),(256,2),(512,1),(512,2)]
try FileManager.default.createDirectory(atPath: output, withIntermediateDirectories: true)
for (points, scale) in sizes {
    let pixels = points * scale
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
                              bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                              isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let factor = CGFloat(pixels) / 1024.0
    NSGraphicsContext.current!.cgContext.scaleBy(x: factor, y: factor)
    let bounds = NSRect(x: 62, y: 62, width: 900, height: 900)
    let rounded = NSBezierPath(roundedRect: bounds, xRadius: 192, yRadius: 192)
    let gradient = NSGradient(starting: NSColor(calibratedRed: 0.12, green: 0.32, blue: 0.75, alpha: 1),
                              ending: NSColor(calibratedRed: 0.08, green: 0.15, blue: 0.38, alpha: 1))!
    gradient.draw(in: rounded, angle: 75)
    let screen = NSBezierPath(roundedRect: NSRect(x: 218, y: 188, width: 588, height: 650), xRadius: 64, yRadius: 64)
    NSColor(calibratedWhite: 0.98, alpha: 1).setFill(); screen.fill()
    let text = "中" as NSString
    let attrs: [NSAttributedString.Key:Any] = [.font: NSFont.systemFont(ofSize: 390, weight: .medium), .foregroundColor: NSColor(calibratedRed: 0.10, green: 0.25, blue: 0.56, alpha: 1)]
    let size = text.size(withAttributes: attrs)
    text.draw(at: NSPoint(x: (1024-size.width)/2, y: 356), withAttributes: attrs)
    let line = NSBezierPath(roundedRect: NSRect(x: 345, y: 270, width: 334, height: 28), xRadius: 14, yRadius: 14)
    NSColor(calibratedRed: 0.25, green: 0.67, blue: 0.48, alpha: 1).setFill(); line.fill()
    NSGraphicsContext.restoreGraphicsState()
    let png = rep.representation(using: .png, properties: [:])!
    let suffix = scale == 2 ? "@2x" : ""
    try png.write(to: URL(fileURLWithPath: output).appendingPathComponent("icon_\(points)x\(points)\(suffix).png"))
}
