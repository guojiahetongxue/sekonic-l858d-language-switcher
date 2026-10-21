import Cocoa

private func argumentValue(_ flag: String) -> String? {
    guard let index = CommandLine.arguments.firstIndex(of: flag), index + 1 < CommandLine.arguments.count else { return nil }
    return CommandLine.arguments[index + 1]
}

private enum UILanguage: String {
    case chinese = "zh", english = "en"
    static var preferred: UILanguage {
        if let override = argumentValue("--ui-language"), let value = UILanguage(rawValue: override) { return value }
        if let saved = UserDefaults.standard.string(forKey: "UILanguage"), let value = UILanguage(rawValue: saved) { return value }
        return Locale.preferredLanguages.first?.hasPrefix("zh") == true ? .chinese : .english
    }
}

private func unsupportedFirmwareMessage(_ language: UILanguage) -> String {
    language == .chinese
        ? "此工具仅支持固件 17、18、19，未修改设备。请退出本工具，通过世光官方软件检查是否可升级至 19；若支持，完成升级后退出官方软件，再回来检测。本工具不升级或降级固件。"
        : "This tool supports firmware 17, 18 and 19 only. No change was made. Quit this app and check Sekonic's official software for an upgrade to 19. If available, complete it, quit the official software, then return to detect your meter. This app does not upgrade or downgrade firmware."
}

private struct DeviceResult {
    let raw: [String: Any]
    var success: Bool { raw["success"] as? Bool ?? false }
    var outcome: String { raw["outcome"] as? String ?? "error" }
    var serial: String? { raw["serial"] as? String }
    var firmware: String? { raw["firmware"] as? String }
    var region: String? { raw["region"] as? String }
    var verificationLevel: String? { raw["verificationLevel"] as? String }
    var firmwareDisplay: String { firmware.flatMap { Int($0) }.map(String.init) ?? firmware ?? "未知" }
    var hasSupportedFirmware: Bool { ["0017", "0018", "0019"].contains(firmware ?? "") }
    var isStaticVerified: Bool { verificationLevel == "static_verified" }
    var languageWriteSupported: Bool { raw["languageWriteSupported"] as? Bool ?? false }
    var supportedLanguages: [Int] { raw["supportedLanguages"] as? [Int] ?? [] }
    var language: Int? { raw["language"] as? Int }
    var targetLanguage: Int? { raw["targetLanguage"] as? Int }
    var setterAttempted: Bool { raw["setterAttempted"] as? Bool ?? false }
    var saveAttempted: Bool { raw["saveAttempted"] as? Bool ?? false }
    var saveAcknowledged: Bool { raw["saveAcknowledged"] as? Bool ?? false }
    var stateMayHaveChanged: Bool { raw["stateMayHaveChanged"] as? Bool ?? false }
    var message: String? { raw["message"] as? String }
    var errorCode: String { raw["errorCode"] as? String ?? "unknown" }

    var canWrite: Bool {
        success && ["inspected", "changed", "already_target"].contains(outcome)
            && hasSupportedFirmware
            && ["0000", "0001", "0002", "0003"].contains(region ?? "")
            && ["hardware_verified", "static_verified"].contains(verificationLevel ?? "")
            && (region == "0002" || verificationLevel == "static_verified")
            && languageWriteSupported && !supportedLanguages.isEmpty
            && supportedLanguages.allSatisfy { [0, 1, 2].contains($0) }
            && Set(supportedLanguages).count == supportedLanguages.count
            && [0, 1, 2].contains(language ?? -1)
            && !(serial ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            && !stateMayHaveChanged
            && (outcome != "changed" || saveAcknowledged)
    }

    func canSet(_ target: Int) -> Bool { canWrite && supportedLanguages.contains(target) && language != target }

    static func decode(_ data: Data) -> DeviceResult? {
        guard let raw = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              raw["success"] is Bool, raw["outcome"] is String else { return nil }
        return DeviceResult(raw: raw)
    }

    static func failure(_ code: String, _ message: String, attemptedWrite: Bool = false) -> DeviceResult {
        DeviceResult(raw: ["success": false, "outcome": "error", "errorCode": code,
                           "message": message, "stateMayHaveChanged": attemptedWrite,
                           "setterAttempted": false, "saveAttempted": false,
                           "saveAcknowledged": false, "events": []])
    }
}

private final class CaptureBox: @unchecked Sendable {
    private let lock = NSLock()
    private var value = Data()
    func set(_ data: Data) { lock.lock(); value = data; lock.unlock() }
    func get() -> Data { lock.lock(); defer { lock.unlock() }; return value }
}

private struct Execution {
    let result: DeviceResult
    let exitStatus: Int32
    let stderr: String
}

private func runHelper(action: String, serial: String?) -> Execution {
    let helper = Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/sekonic-device")
    guard FileManager.default.isExecutableFile(atPath: helper.path) else {
        return Execution(result: .failure("helper_missing", "应用中的设备组件不完整，请重新解压完整应用。"), exitStatus: -1, stderr: "")
    }
    let process = Process()
    process.executableURL = helper
    process.arguments = [action] + (serial.map { ["--serial", $0] } ?? [])
    let outPipe = Pipe(), errPipe = Pipe()
    process.standardOutput = outPipe
    process.standardError = errPipe
    do { try process.run() } catch {
        return Execution(result: .failure("launch_failed", "无法启动设备组件，请重新解压完整应用后再试。"), exitStatus: -1, stderr: error.localizedDescription)
    }
    let group = DispatchGroup()
    let output = CaptureBox(), errors = CaptureBox()
    group.enter()
    DispatchQueue.global(qos: .userInitiated).async {
        output.set(outPipe.fileHandleForReading.readDataToEndOfFile()); group.leave()
    }
    group.enter()
    DispatchQueue.global(qos: .userInitiated).async {
        errors.set(errPipe.fileHandleForReading.readDataToEndOfFile()); group.leave()
    }
    process.waitUntilExit()
    group.wait()
    let status = process.terminationStatus
    let parsed = DeviceResult.decode(output.get())
    let result: DeviceResult
    if let parsed = parsed {
        // A crashing helper must not turn a partial success response into a green result.
        if parsed.success && status != 0 {
            var raw = parsed.raw
            raw["success"] = false
            raw["outcome"] = "error"
            raw["errorCode"] = "unexpected_exit"
            raw["message"] = "设备组件意外结束，请查看操作记录并重新检测。"
            raw["stateMayHaveChanged"] = action != "inspect"
            result = DeviceResult(raw: raw)
        } else { result = parsed }
    } else {
        result = .failure("invalid_response", "设备组件未返回完整结果，请查看操作记录。", attemptedWrite: action != "inspect")
    }
    return Execution(result: result, exitStatus: status,
                     stderr: String(data: errors.get(), encoding: .utf8) ?? "")
}

private final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var window: NSWindow!
    private var lastResult: DeviceResult?
    private var lastAction = "inspect"
    private var uiLanguage = UILanguage.preferred
    private var lastLogFailed = false
    private var busy = false
    private var busyAlertVisible = false
    private var demoLanguage = 1
    private let demo = CommandLine.arguments.contains("--demo")
    private let demoFirmware18 = CommandLine.arguments.contains("--demo-firmware18")
    private let demoFirmware19 = CommandLine.arguments.contains("--demo-firmware19")
    private let demoRegion = argumentValue("--demo-region") ?? "0002"
    private let ink = NSColor(calibratedRed: 0.15, green: 0.19, blue: 0.23, alpha: 1)
    private let muted = NSColor(calibratedRed: 0.43, green: 0.47, blue: 0.50, alpha: 1)
    private let blue = NSColor(calibratedRed: 0.16, green: 0.37, blue: 0.68, alpha: 1)
    private let green = NSColor(calibratedRed: 0.15, green: 0.42, blue: 0.30, alpha: 1)
    private let orange = NSColor(calibratedRed: 0.56, green: 0.32, blue: 0.12, alpha: 1)
    private let serialValue = NSTextField(labelWithString: "—")
    private let firmwareValue = NSTextField(labelWithString: "—")
    private let regionValue = NSTextField(labelWithString: "—")
    private let verificationValue = NSTextField(labelWithString: "—")
    private let languageValue = NSTextField(labelWithString: "—")
    private let badge = NSTextField(labelWithString: "等待连接")
    private let statusTitle = NSTextField(wrappingLabelWithString: "先连接测光表")
    private let statusDetail = NSTextField(wrappingLabelWithString: "使用 USB 数据线连接测光表并开机，退出世光 Data Transfer Software，然后点击“检测测光表”。")
    private let logWarning = NSTextField(wrappingLabelWithString: "")
    private let progress = NSProgressIndicator()
    private var detectButton: NSButton!
    private var chineseButton: NSButton!
    private var englishButton: NSButton!
    private var japaneseButton: NSButton!
    private var logButton: NSButton!
    private var helpButton: NSButton!
    private var interfacePicker: NSPopUpButton!

    private func t(_ chinese: String, _ english: String) -> String { uiLanguage == .chinese ? chinese : english }
    private var appName: String { t("858D 语言切换", "858D Language Switcher") }

    func applicationDidFinishLaunching(_ notification: Notification) {
        if demo, let value = argumentValue("--demo-language"), let number = Int(value), [0, 1, 2].contains(number) { demoLanguage = number }
        NSApp.appearance = NSAppearance(named: .aqua)
        installMenu()
        buildWindow()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        if demo, let index = CommandLine.arguments.firstIndex(of: "--demo-state"),
           index + 1 < CommandLine.arguments.count {
            showDemoState(CommandLine.arguments[index + 1])
        }
    }

    private func installMenu() {
        let menu = NSMenu()
        let appItem = NSMenuItem()
        menu.addItem(appItem)
        let appMenu = NSMenu()
        appItem.submenu = appMenu
        appMenu.addItem(withTitle: t("关于 \(appName)", "About \(appName)"), action: #selector(showAbout), keyEquivalent: "")
        appMenu.addItem(NSMenuItem.separator())
        appMenu.addItem(withTitle: t("使用说明", "Instructions"), action: #selector(openHelp), keyEquivalent: "?")
        appMenu.addItem(NSMenuItem.separator())
        appMenu.addItem(withTitle: t("退出 \(appName)", "Quit \(appName)"), action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        NSApp.mainMenu = menu
    }

    private func label(_ text: String, size: CGFloat = 13, weight: NSFont.Weight = .regular,
                       color: NSColor? = nil) -> NSTextField {
        let view = NSTextField(labelWithString: text)
        view.font = NSFont.systemFont(ofSize: size, weight: weight)
        view.textColor = color ?? ink
        view.lineBreakMode = .byTruncatingTail
        return view
    }

    private func stack(_ views: [NSView], vertical: Bool, spacing: CGFloat = 12) -> NSStackView {
        let result = NSStackView(views: views)
        result.orientation = vertical ? .vertical : .horizontal
        result.alignment = vertical ? .leading : .centerY
        result.spacing = spacing
        return result
    }

    private func stretch() -> NSView {
        let view = NSView()
        view.setContentHuggingPriority(.init(1), for: .horizontal)
        view.setContentCompressionResistancePriority(.init(1), for: .horizontal)
        return view
    }

    private func card(_ contents: NSView, color: NSColor = .white, padding: CGFloat = 20) -> NSView {
        let outer = NSView()
        outer.wantsLayer = true
        outer.layer?.backgroundColor = color.cgColor
        outer.layer?.cornerRadius = 14
        contents.translatesAutoresizingMaskIntoConstraints = false
        outer.addSubview(contents)
        NSLayoutConstraint.activate([
            contents.leadingAnchor.constraint(equalTo: outer.leadingAnchor, constant: padding),
            contents.trailingAnchor.constraint(equalTo: outer.trailingAnchor, constant: -padding),
            contents.topAnchor.constraint(equalTo: outer.topAnchor, constant: padding),
            contents.bottomAnchor.constraint(equalTo: outer.bottomAnchor, constant: -padding)
        ])
        return outer
    }

    private func button(_ title: String, action: Selector) -> NSButton {
        let b = NSButton(title: title, target: self, action: action)
        b.bezelStyle = .rounded
        b.controlSize = .large
        b.font = NSFont.systemFont(ofSize: 13, weight: .medium)
        return b
    }

    private func buildWindow() {
        if window == nil {
            window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 720, height: 780),
                              styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
            window.isReleasedWhenClosed = false
            window.delegate = self
            window.center()
        }
        window.contentView = NSView(frame: NSRect(x: 0, y: 0, width: 720, height: 780))
        window.title = appName
        window.titlebarAppearsTransparent = true
        window.backgroundColor = NSColor(calibratedRed: 0.96, green: 0.96, blue: 0.95, alpha: 1)
        badge.stringValue = t("等待连接", "Waiting for a meter")
        statusTitle.stringValue = t("先连接测光表", "Connect your meter")
        statusDetail.stringValue = t("使用 USB 数据线连接测光表并开机，退出世光 Data Transfer Software，然后点击“检测测光表”。",
                                    "Connect and turn on your meter with a USB data cable. Quit Sekonic Data Transfer Software, then click Detect meter.")

        let heading = label(appName, size: 27, weight: .semibold)
        let subtitle = label(t("中文、English、日本語，使用设备内置语言。", "Choose Chinese, English or Japanese built into your meter."), size: 14, color: muted)
        let intro = stack([heading, subtitle], vertical: true, spacing: 8)

        detectButton = button(t("检测测光表", "Detect meter"), action: #selector(inspectDevice))
        badge.font = .systemFont(ofSize: 11, weight: .medium)
        badge.textColor = muted
        let modelLabels = stack([label("SEKONIC L-858D", size: 15, weight: .semibold), badge], vertical: true, spacing: 5)
        let cardHeader = stack([modelLabels, stretch(), detectButton], vertical: false)
        let divider = NSBox()
        divider.boxType = .separator
        for value in [serialValue, firmwareValue, regionValue, verificationValue, languageValue] {
            value.font = .systemFont(ofSize: 14, weight: .medium)
            value.textColor = ink
            value.isSelectable = true
        }
        serialValue.font = .monospacedSystemFont(ofSize: 13, weight: .medium)
        let grid = NSGridView(views: [
            [label(t("序列号", "Serial number"), color: muted), serialValue],
            [label(t("固件版本", "Firmware"), color: muted), firmwareValue],
            [label(t("设备地区", "Device region"), color: muted), regionValue],
            [label(t("验证情况", "Verification"), color: muted), verificationValue],
            [label(t("当前语言", "Current language"), color: muted), languageValue]
        ])
        grid.rowSpacing = 10
        grid.columnSpacing = 30
        grid.column(at: 0).width = 122
        grid.column(at: 1).xPlacement = .leading
        let deviceStack = stack([cardHeader, divider, grid], vertical: true, spacing: 17)
        for view in [cardHeader, divider, grid] { view.widthAnchor.constraint(equalTo: deviceStack.widthAnchor).isActive = true }
        let deviceCard = card(deviceStack)

        progress.style = .spinning
        progress.controlSize = .small
        progress.isDisplayedWhenStopped = false
        progress.widthAnchor.constraint(equalToConstant: 16).isActive = true
        progress.heightAnchor.constraint(equalToConstant: 16).isActive = true
        statusTitle.font = .systemFont(ofSize: 14, weight: .semibold)
        statusTitle.textColor = ink
        statusDetail.font = .systemFont(ofSize: 12)
        statusDetail.textColor = muted
        statusDetail.maximumNumberOfLines = 0
        statusDetail.setContentCompressionResistancePriority(.required, for: .vertical)
        let statusHeading = stack([statusTitle, stretch(), progress], vertical: false, spacing: 8)
        let statusStack = stack([statusHeading, statusDetail], vertical: true, spacing: 7)
        statusHeading.widthAnchor.constraint(equalTo: statusStack.widthAnchor).isActive = true
        statusDetail.widthAnchor.constraint(equalTo: statusStack.widthAnchor).isActive = true
        let statusCard = card(statusStack, color: NSColor(calibratedRed: 0.91, green: 0.93, blue: 0.94, alpha: 1), padding: 16)
        statusCard.heightAnchor.constraint(greaterThanOrEqualToConstant: 92).isActive = true

        chineseButton = button("中文", action: #selector(setChinese))
        chineseButton.bezelColor = blue
        chineseButton.contentTintColor = .white
        englishButton = button("English", action: #selector(setEnglish))
        japaneseButton = button("日本語", action: #selector(setJapanese))
        let languageButtons = stack([chineseButton, englishButton, japaneseButton], vertical: false, spacing: 12)
        languageButtons.distribution = .fillEqually
        for target in [chineseButton!, englishButton!, japaneseButton!] {
            target.heightAnchor.constraint(equalToConstant: 36).isActive = true
        }
        let actionTitle = label(t("点击目标语言，即切换并保存", "Click a language to switch and save"), size: 12, color: muted)
        let actions = stack([actionTitle, languageButtons], vertical: true, spacing: 7)
        languageButtons.widthAnchor.constraint(equalTo: actions.widthAnchor).isActive = true

        let support = NSTextField(wrappingLabelWithString: demo
            ? t("演示模式 · 不连接设备，也不修改测光表", "DEMO · No device connection or changes")
            : t("同一台日本地区测光表的固件 17、18、19 已有实机记录，各目标的验证范围详见说明。其他已审核地区尚未实机验证。",
                "Firmware 17/18/19 has test records on one Japan-region meter. See Instructions for target-specific coverage. Other supported regions have protocol review only."))
        support.font = .systemFont(ofSize: 11)
        support.textColor = demo ? orange : muted
        logWarning.font = .systemFont(ofSize: 11)
        logWarning.textColor = orange
        logWarning.maximumNumberOfLines = 0
        logWarning.stringValue = logFailureText
        logWarning.isHidden = !lastLogFailed

        logButton = NSButton(title: t("查看操作记录", "Show logs"), target: self, action: #selector(openLogs))
        helpButton = NSButton(title: t("使用说明", "Instructions"), target: self, action: #selector(openHelp))
        for b in [logButton!, helpButton!] {
            b.isBordered = false
            b.font = .systemFont(ofSize: 12)
            b.contentTintColor = blue
        }
        interfacePicker = NSPopUpButton(frame: .zero, pullsDown: false)
        interfacePicker.addItems(withTitles: ["中文", "English"])
        interfacePicker.selectItem(at: uiLanguage == .chinese ? 0 : 1)
        interfacePicker.target = self
        interfacePicker.action = #selector(changeInterfaceLanguage)
        interfacePicker.controlSize = .small
        interfacePicker.toolTip = t("只更改电脑上的界面语言", "Changes this app's interface only")
        let pickerLabel = label(t("界面", "App language"), size: 11, color: muted)
        let footer = stack([helpButton, stretch(), pickerLabel, interfacePicker, logButton], vertical: false, spacing: 9)
        let privacy = label(t("操作记录仅保存在本机，分享前请隐藏序列号。", "Logs stay on this Mac. Hide serial numbers before sharing."), size: 10, color: muted)
        let page = stack([intro, deviceCard, statusCard, actions, support, logWarning, footer, privacy], vertical: true, spacing: 14)
        page.setCustomSpacing(9, after: support)
        page.setCustomSpacing(4, after: footer)
        page.translatesAutoresizingMaskIntoConstraints = false
        guard let content = window.contentView else { return }
        content.addSubview(page)
        NSLayoutConstraint.activate([
            page.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 28),
            page.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -28),
            page.topAnchor.constraint(equalTo: content.topAnchor, constant: 20),
            page.bottomAnchor.constraint(lessThanOrEqualTo: content.bottomAnchor, constant: -18)
        ])
        for view in [intro, deviceCard, statusCard, actions, support, logWarning, footer, privacy] {
            view.widthAnchor.constraint(equalTo: page.widthAnchor).isActive = true
        }
        updateButtons()
    }

    @objc private func changeInterfaceLanguage() {
        guard !busy else { return }
        uiLanguage = interfacePicker.indexOfSelectedItem == 0 ? .chinese : .english
        if !demo { UserDefaults.standard.set(uiLanguage.rawValue, forKey: "UILanguage") }
        installMenu()
        buildWindow()
        if let result = lastResult { renderResult(result, action: lastAction) }
    }

    private var logFailureText: String {
        t("本次操作记录未能保存，请保留此窗口中的设备结果。", "The operation log could not be saved. Keep a copy of the device result shown here.")
    }

    private func languageName(_ value: Int?) -> String {
        switch value { case 0: return t("English（英文）", "English"); case 1: return t("日本語（日文）", "日本語 (Japanese)"); case 2: return t("中文", "中文 (Chinese)"); default: return t("未能确认", "Not confirmed") }
    }

    private func regionName(_ value: String?) -> String {
        guard let value = value else { return "—" }
        switch value {
        case "0000": return t("0000 · 美国 U", "0000 · US / U")
        case "0001": return t("0001 · 其他已核验地区", "0001 · Other reviewed region")
        case "0002": return t("0002 · 日本", "0002 · Japan")
        case "0003": return t("0003 · 中文默认地区", "0003 · Chinese-default region")
        default: return t("\(value) · 未核验", "\(value) · Not reviewed")
        }
    }

    private func showDevice(_ result: DeviceResult) {
        serialValue.stringValue = result.serial ?? "—"
        firmwareValue.stringValue = result.firmware == nil ? "—" : result.firmwareDisplay
        regionValue.stringValue = regionName(result.region)
        languageValue.stringValue = languageName(result.language)
        verificationValue.font = .systemFont(ofSize: 12, weight: .medium)
        if result.isStaticVerified {
            verificationValue.stringValue = t("协议已核验 · 尚未实机验证", "Protocol reviewed; not hardware-tested")
            verificationValue.textColor = orange
        } else if result.verificationLevel == "hardware_verified" && result.region == "0002" {
            verificationValue.stringValue = t("该配置已有实机记录，详见说明", "Hardware records exist; see Instructions")
            verificationValue.textColor = green
        } else {
            verificationValue.stringValue = t("未核验", "Not reviewed")
            verificationValue.textColor = muted
        }
        badge.stringValue = result.canWrite
            ? t("已识别 · 固件 \(result.firmwareDisplay)", "Detected · Firmware \(result.firmwareDisplay)")
            : (result.serial == nil ? t("尚未识别", "Not detected") : t("已识别 · 修改暂不可用", "Detected · Changes unavailable"))
        badge.textColor = result.canWrite ? (result.isStaticVerified ? orange : green) : muted
        languageValue.textColor = result.language == 2 ? green : ink
    }

    private func updateButtons() {
        detectButton.isEnabled = !busy
        chineseButton.isEnabled = !busy && lastResult?.canSet(2) == true
        englishButton.isEnabled = !busy && lastResult?.canSet(0) == true
        japaneseButton.isEnabled = !busy && lastResult?.canSet(1) == true
        chineseButton.title = lastResult?.language == 2 && lastResult?.success == true ? "✓ 中文" : "中文"
        englishButton.title = lastResult?.language == 0 && lastResult?.success == true ? "✓ English" : "English"
        japaneseButton.title = lastResult?.language == 1 && lastResult?.success == true ? "✓ 日本語" : "日本語"
        logButton.isEnabled = !busy
        helpButton.isEnabled = !busy
        interfacePicker.isEnabled = !busy
        window.standardWindowButton(.closeButton)?.isEnabled = !busy
    }

    @objc private func inspectDevice() { execute("inspect", serial: nil) }
    @objc private func setChinese() {
        guard lastResult?.canSet(2) == true, let serial = lastResult?.serial else { return }
        execute("set-chinese", serial: serial)
    }
    @objc private func setEnglish() {
        guard lastResult?.canSet(0) == true, let serial = lastResult?.serial else { return }
        execute("set-english", serial: serial)
    }
    @objc private func setJapanese() {
        guard lastResult?.canSet(1) == true, let serial = lastResult?.serial else { return }
        execute("set-japanese", serial: serial)
    }

    private var logDirectory: URL {
        if demo, let override = ProcessInfo.processInfo.environment["L858D_LOG_DIR"], !override.isEmpty {
            return URL(fileURLWithPath: override, isDirectory: true)
        }
        return FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("L858D Language Switcher/Logs", isDirectory: true)
    }

    private func writeLog(_ execution: Execution, action: String, expectedSerial: String?) -> String? {
        do {
            let directory = logDirectory
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                                    attributes: [.posixPermissions: 0o700])
            let formatter = ISO8601DateFormatter()
            formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
            let stamp = formatter.string(from: Date())
            var record: [String: Any] = ["timestamp": stamp, "action": action, "demo": demo,
                                         "helperExitStatus": execution.exitStatus,
                                         "result": execution.result.raw,
                                         "applicationVersion": Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "1.0"]
            if let serial = expectedSerial { record["expectedSerial"] = serial }
            if !execution.stderr.isEmpty { record["helperStderr"] = execution.stderr }
            let file = directory.appendingPathComponent("\(stamp.replacingOccurrences(of: ":", with: "-"))-\(action)-\(UUID().uuidString.prefix(8)).json")
            let data = try JSONSerialization.data(withJSONObject: record, options: [.prettyPrinted, .sortedKeys])
            try data.write(to: file, options: .atomic)
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file.path)
            return nil
        } catch { return "本次操作记录未能保存。设备结果如下，请保留此窗口信息。" }
    }

    private func execute(_ action: String, serial: String?) {
        guard !busy else { return }
        busy = true
        lastAction = action
        updateButtons()
        lastLogFailed = false
        logWarning.isHidden = true
        progress.startAnimation(nil)
        statusTitle.textColor = ink
        statusTitle.stringValue = action == "inspect" ? t("正在检测测光表…", "Detecting meter…") : t("正在切换并保存…", "Switching and saving…")
        statusDetail.stringValue = action == "inspect"
            ? t("正在确认型号、固件、地区和当前语言。", "Checking model, firmware, region and current language.")
            : t("请保持 USB 连接和测光表供电，等待操作完成。", "Keep the USB cable connected and the meter powered on until this finishes.")
        let mock = demo ? mockResult(action: action) : nil
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            let execution: Execution
            if let mock = mock {
                Thread.sleep(forTimeInterval: 0.35)
                execution = Execution(result: mock, exitStatus: mock.success ? 0 : 1, stderr: "")
            } else { execution = runHelper(action: action, serial: serial) }
            let warning = self.writeLog(execution, action: action, expectedSerial: serial)
            DispatchQueue.main.async { [weak self] in
                guard let self = self else { return }
                self.busy = false
                self.progress.stopAnimation(nil)
                self.renderResult(execution.result, action: action)
                if warning != nil {
                    self.lastLogFailed = true
                    self.logWarning.stringValue = self.logFailureText
                    self.logWarning.isHidden = false
                }
            }
        }
    }

    private func renderResult(_ result: DeviceResult, action: String) {
        lastResult = result
        lastAction = action
        showDevice(result)
        statusTitle.textColor = ink
        if result.success && result.outcome == "inspected" && result.canWrite {
            statusTitle.stringValue = result.isStaticVerified
                ? t("协议已核验，尚未实机验证", "Protocol reviewed; hardware test pending")
                : t("可以切换语言", "Ready to switch language")
            let nextStep = t("点击下方可用语言即切换并保存，完成后请拔线、关机重启确认。",
                             "Click an available language below to switch and save. Then disconnect USB and restart the meter to check the menu.")
            let summary = result.isStaticVerified
                ? t("此固件及地区的命令路径已核验，但该地区尚未完成实机测试。",
                    "This firmware and region passed protocol review, but this region has not been tested on a meter.")
                : t("该配置已有实机记录，各目标语言的验证范围详见说明。",
                    "This configuration has hardware test records. See Instructions for the coverage of each target language.")
            let detected = uiLanguage == .chinese && result.message?.isEmpty == false
                ? String(result.message!.prefix(180)) : summary
            statusDetail.stringValue = detected + " " + nextStep
            statusTitle.textColor = result.isStaticVerified ? orange : green
        } else if result.success && result.outcome == "changed" && result.saveAcknowledged {
            let language = languageName(result.targetLanguage ?? result.language)
            statusTitle.stringValue = t("已保存为\(language)", "Saved: \(language)")
            statusDetail.stringValue = t("请断开 USB，将测光表关机后重新开机，确认菜单已显示\(language)。",
                                        "Disconnect USB, turn the meter off and on, then confirm the menu displays \(language).")
            statusTitle.textColor = green
            badge.stringValue = t("已保存 · 等待重启确认", "Saved · Restart to confirm")
        } else if result.success && result.outcome == "already_target" {
            let language = languageName(result.targetLanguage ?? result.language)
            statusTitle.stringValue = t("已经是\(language)", "Already set to \(language)")
            statusDetail.stringValue = t("未重复修改设置。请断开 USB 并重启测光表，确认菜单显示。",
                                        "No repeated write was made. Disconnect USB and restart the meter to confirm the menu.")
            statusTitle.textColor = green
        } else if result.stateMayHaveChanged || result.setterAttempted || result.saveAttempted {
            statusTitle.stringValue = t("操作已停止，结果需要确认", "Stopped; result needs checking")
            statusTitle.textColor = orange
            let fallback = result.saveAcknowledged
                ? t("设备已确认保存，但后续验证未完成。请查看操作记录，断开 USB 并关机重启，再重新检测。",
                    "The meter acknowledged saving, but final verification did not complete. Review the log, disconnect USB, restart the meter, then detect it again.")
                : t("设备语言可能已改变，保存状态尚未确认。请查看操作记录，断开 USB 并关机重启，再重新检测，不要直接重复切换。",
                    "The meter language may have changed, but saving is not confirmed. Review the log, disconnect USB, restart the meter, then detect it again. Do not repeat the change immediately.")
            if uiLanguage == .chinese, let message = result.message, !message.isEmpty {
                statusDetail.stringValue = String(message.prefix(180)) + " 请查看操作记录，断开 USB 并关机重启后再重新检测。"
            } else { statusDetail.stringValue = fallback }
            badge.stringValue = t("需要重新检测", "Detect again after restart")
        } else if result.outcome == "unsupported" || (result.success && result.outcome == "inspected" && !result.canWrite) {
            statusTitle.stringValue = t("已识别设备，暂不支持修改", "Detected; changes unavailable")
            statusTitle.textColor = orange
            if result.firmware != nil && !result.hasSupportedFirmware {
                statusDetail.stringValue = unsupportedFirmwareMessage(uiLanguage)
            } else if result.outcome == "unsupported" { statusDetail.stringValue = friendlyError(result) }
            else {
                statusDetail.stringValue = t("设备组件未提供一致的修改权限或兼容信息。本次未请求修改，请查看操作记录。",
                                            "The device component did not provide matching capability and compatibility information. No change was requested. Review the log.")
            }
        } else {
            statusTitle.stringValue = action == "inspect"
                ? t("未完成检测", "Detection did not complete") : t("未完成切换", "Change did not complete")
            statusTitle.textColor = orange
            statusDetail.stringValue = friendlyError(result)
        }
        updateButtons()
    }

    private func friendlyError(_ result: DeviceResult) -> String {
        let code = result.errorCode.lowercased()
        if code == "unsupported_firmware" { return unsupportedFirmwareMessage(uiLanguage) }
        if code.contains("no_device") || code.contains("not_found") {
            return t("未找到测光表。请确认使用 USB 数据线连接并开机，再点击“检测测光表”。",
                     "No meter was found. Connect it using a USB data cable, turn it on, then click Detect meter.")
        }
        if code.contains("multiple") {
            return t("检测到多台测光表。请只保留一台连接，再重新检测。",
                     "More than one meter was detected. Connect only one meter, then detect again.")
        }
        if code.contains("serial") || code.contains("identity") {
            return t("设备身份与预期不一致。请重新检测后再操作。",
                     "The meter identity does not match the detected device. Detect the meter again before making a change.")
        }
        if code.contains("busy") || code.contains("open") || code.contains("permission") {
            return t("无法访问测光表。请退出世光 Data Transfer Software，检查连接后重新检测。",
                     "The meter could not be opened. Quit Sekonic Data Transfer Software, check the cable, then detect again.")
        }
        if uiLanguage == .chinese, let message = result.message, !message.isEmpty { return String(message.prefix(230)) }
        switch code {
        case "unsupported_region":
            return "This region has not been reviewed. No language change was made. Save the log for further analysis."
        case "unsupported_language":
            return "The current language value is not recognized. No change was made. Save the log for further analysis."
        case "helper_missing", "launch_failed":
            return "The device component is missing or could not start. Extract a fresh copy of the complete app."
        case "unexpected_exit", "invalid_response":
            return "The device component did not return a complete result. Review the log and detect the meter again."
        case "reply_timeout":
            return "The meter did not reply in time. Check its power and cable, quit other meter software, then detect again."
        case "unexpected_reply_value", "language_readback_mismatch", "unsolicited_or_duplicate_input":
            return "The meter returned an unexpected response. The operation stopped. Review the log before trying again."
        default:
            return "The operation could not be confirmed. Review the local log for the original device message, then detect again."
        }
    }

    private func mockResult(action: String) -> DeviceResult {
        var raw: [String: Any] = ["success": true, "outcome": "inspected", "serial": "JY10-DEMO001",
                                   "firmware": demoFirmware19 ? "0019" : (demoFirmware18 ? "0018" : "0017"), "region": demoRegion, "language": demoLanguage,
                                   "verificationLevel": demoRegion == "0002" ? "hardware_verified" : "static_verified",
                                   "languageWriteSupported": true, "supportedLanguages": [0, 1, 2],
                                   "message": demoRegion == "0002" ? "设备检查通过，可切换中文、英文或日文。该配置已有实机记录，各目标验证范围详见说明。"
                                    : "此固件及地区的命令路径已核验，但该地区尚未完成实机测试。",
                                   "setterAttempted": false, "saveAttempted": false, "saveAcknowledged": false,
                                   "stateMayHaveChanged": false, "events": [["event": "demo_only"]]]
        if action != "inspect" {
            guard let target = ["set-chinese": 2, "set-english": 0, "set-japanese": 1][action] else {
                return .failure("invalid_demo_action", "无效的演示操作。")
            }
            let changed = target != demoLanguage
            demoLanguage = target
            raw["language"] = target
            raw["targetLanguage"] = target
            raw["outcome"] = changed ? "changed" : "already_target"
            raw["setterAttempted"] = changed
            raw["saveAttempted"] = changed
            raw["saveAcknowledged"] = changed
            raw["stateMayHaveChanged"] = changed
        }
        return DeviceResult(raw: raw)
    }

    private func showDemoState(_ state: String) {
        if state == "english" { demoLanguage = 0 }
        var result = mockResult(action: "inspect")
        var action = "inspect"
        if state == "success" { result = mockResult(action: "set-chinese"); action = "set-chinese" }
        else if state == "english-success" { result = mockResult(action: "set-english"); action = "set-english" }
        else if state == "unsupported" {
            var raw = result.raw; raw["success"] = false; raw["outcome"] = "unsupported"; raw["firmware"] = "0016"
            raw["message"] = unsupportedFirmwareMessage(.chinese)
            raw["errorCode"] = "unsupported_firmware"
            raw["languageWriteSupported"] = false; raw["supportedLanguages"] = []
            raw.removeValue(forKey: "verificationLevel")
            result = DeviceResult(raw: raw)
        } else if state == "unsupported-region" {
            var raw = result.raw; raw["success"] = false; raw["outcome"] = "unsupported"; raw["region"] = "0004"
            raw["message"] = "此地区未完成核验，未修改设备。"; raw["errorCode"] = "unsupported_region"
            raw["languageWriteSupported"] = false; raw["supportedLanguages"] = []
            raw.removeValue(forKey: "verificationLevel")
            result = DeviceResult(raw: raw)
        } else if state == "missing-capability" {
            var raw = result.raw; raw.removeValue(forKey: "languageWriteSupported"); raw.removeValue(forKey: "supportedLanguages")
            result = DeviceResult(raw: raw)
        } else if state == "error" {
            var raw = result.raw; raw["success"] = false; raw["outcome"] = "error"; raw["stateMayHaveChanged"] = true
            raw["setterAttempted"] = true; raw["errorCode"] = "demo_readback_failed"
            raw["message"] = "已发送语言修改命令，但尚未完成保存。当前语言可能已改变，请拔线重启后重新检测。"
            raw["languageWriteSupported"] = false; raw["supportedLanguages"] = []
            result = DeviceResult(raw: raw); action = "set-chinese"
        }
        renderResult(result, action: action)
    }

    @objc private func openLogs() {
        guard !busy else { return }
        do {
            try FileManager.default.createDirectory(at: logDirectory, withIntermediateDirectories: true,
                                                    attributes: [.posixPermissions: 0o700])
            NSWorkspace.shared.open(logDirectory)
        } catch { showMessage(t("无法打开操作记录", "Cannot open logs"), t("记录目录无法创建或访问。", "The log folder could not be created or opened.") + "\n\(logDirectory.path)") }
    }

    @objc private func openHelp() {
        guard !busy else { return }
        if let url = Bundle.main.url(forResource: uiLanguage == .chinese ? "使用说明" : "Read Me", withExtension: "txt") {
            NSWorkspace.shared.open(url)
        } else { showMessage(t("使用说明", "Instructions"), t("连接并开启测光表，退出世光软件，点击检测。仅支持固件 17、18、19，且地区须经审核、设备组件允许修改。其他版本请退出本工具，用世光官方软件检查是否可升级至 19；若支持，完成升级并退出官方软件后再回来检测。选择目标语言即切换并保存，完成后断开 USB 并重启确认。同一台日本地区测光表已有这三个版本的实机记录，各目标验证范围见说明；其他已审核地区仅完成协议核验。本工具不升级或降级固件。",
                                                            "Connect and turn on the meter, quit Sekonic software, then detect it. Only firmware 17, 18 and 19 with a reviewed region and permission from the device component can be changed. For other versions, quit this app and check Sekonic's official software for an upgrade to 19. If available, complete it, quit the official software, then return. Click a target language to switch and save, then disconnect USB and restart. These three versions have test records on one Japan-region meter; see Instructions for each target's coverage. Other supported regions have protocol review only. This app does not upgrade or downgrade firmware.")) }
    }

    @objc private func showAbout() {
        guard !busy else { return }
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.4.0"
        showMessage(appName, t("版本 \(version)\n独立工具，非世光官方产品。\n同一台日本地区测光表的固件 17、18、19 已有实机记录，各目标语言的验证范围详见说明。其他已审核地区目前仅完成协议核验，尚未实机验证。本工具不升级或降级固件。",
                              "Version \(version)\nAn independent tool, not an official Sekonic product.\nFirmware 17/18/19 has hardware test records on one Japan-region meter. See Instructions for each target's coverage. Other supported regions have protocol review only and have not been hardware-tested. This app does not upgrade or downgrade firmware."))
    }

    private func showMessage(_ title: String, _ detail: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = detail
        alert.addButton(withTitle: t("知道了", "OK"))
        alert.beginSheetModal(for: window)
    }

    private func explainBusy() {
        guard !busyAlertVisible else { return }
        busyAlertVisible = true
        let alert = NSAlert()
        alert.messageText = t("请等待当前操作完成", "Wait for the current operation")
        alert.informativeText = t("正在与测光表通信，请保持 USB 连接。操作结束后即可关闭应用。", "The app is communicating with the meter. Keep USB connected. You can close the app when the operation finishes.")
        alert.addButton(withTitle: t("继续等待", "Keep waiting"))
        alert.beginSheetModal(for: window) { [weak self] _ in self?.busyAlertVisible = false }
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        if busy { explainBusy(); return false }
        return true
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if busy { explainBusy(); return .terminateCancel }
        return .terminateNow
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

private func selfCheck() -> Bool {
    let base: [String: Any] = ["success": true, "outcome": "inspected", "serial": "JY10-TEST",
                               "firmware": "0017", "region": "0002", "language": 1,
                               "verificationLevel": "hardware_verified", "languageWriteSupported": true,
                               "supportedLanguages": [0, 1, 2]]
    for firmware in ["0017", "0018", "0019"] {
        for region in ["0000", "0001", "0002", "0003"] {
            for language in [0, 1, 2] {
                var raw = base
                raw["firmware"] = firmware; raw["region"] = region; raw["language"] = language
                raw["verificationLevel"] = region == "0002" ? "hardware_verified" : "static_verified"
                let result = DeviceResult(raw: raw)
                guard result.canWrite, result.isStaticVerified == (region != "0002") else { return false }
                for target in [0, 1, 2] {
                    guard result.canSet(target) == (target != language) else { return false }
                }
            }
        }
    }
    for firmware in ["0010", "0012", "0013", "0014", "0015", "0016", "0020", "0000", "FFFF", ""] {
        var raw = base; raw["firmware"] = firmware
        guard !DeviceResult(raw: raw).canWrite else { return false }
    }
    for (key, value) in [("region", "0004" as Any), ("region", "FFFF" as Any), ("language", 3 as Any),
                         ("serial", "" as Any), ("success", false as Any), ("stateMayHaveChanged", true as Any),
                         ("verificationLevel", "unverified" as Any), ("languageWriteSupported", false as Any),
                         ("supportedLanguages", [] as [Int]), ("supportedLanguages", [0, 3]),
                         ("supportedLanguages", [0, 0])] {
        var raw = base; raw[key] = value
        guard !DeviceResult(raw: raw).canWrite else { return false }
    }
    for key in ["languageWriteSupported", "supportedLanguages", "verificationLevel"] {
        var raw = base; raw.removeValue(forKey: key)
        guard !DeviceResult(raw: raw).canWrite else { return false }
    }
    var subset = base; subset["supportedLanguages"] = [0, 2]
    guard DeviceResult(raw: subset).canSet(0), DeviceResult(raw: subset).canSet(2),
          !DeviceResult(raw: subset).canSet(1) else { return false }
    var invalidLevel = base; invalidLevel["region"] = "0000"
    guard !DeviceResult(raw: invalidLevel).canWrite else { return false }
    var staticJapan = base; staticJapan["verificationLevel"] = "static_verified"
    guard DeviceResult(raw: staticJapan).canWrite, DeviceResult(raw: staticJapan).isStaticVerified else { return false }
    var changed = base; changed["outcome"] = "changed"
    guard !DeviceResult(raw: changed).canWrite else { return false }
    changed["saveAcknowledged"] = true
    guard DeviceResult(raw: changed).canWrite else { return false }
    changed["stateMayHaveChanged"] = true
    guard !DeviceResult(raw: changed).canWrite else { return false }
    guard DeviceResult.decode(Data("{}".utf8)) == nil,
          let encoded = try? JSONSerialization.data(withJSONObject: base),
          DeviceResult.decode(encoded)?.canWrite == true else { return false }
    return true
}

if CommandLine.arguments.contains("--self-check") {
    let passed = selfCheck()
    print(passed ? "UI self-check passed (firmware 17/18/19, three languages, capabilities, four reviewed regions, and unsupported cases)" : "UI self-check failed")
    exit(passed ? 0 : 1)
}

let application = NSApplication.shared
private let delegate = AppDelegate()
application.setActivationPolicy(.regular)
application.delegate = delegate
application.run()
