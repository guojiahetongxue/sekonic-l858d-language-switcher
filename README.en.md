# L858D Language Switcher

[中文](README.md) · **English**

Switch an eligible Sekonic **L-858D / L-858D-U** to **Simplified Chinese, English or Japanese** from your Mac. The app selects languages already present in the meter and saves the setting. Its own interface can independently use English or Chinese.

This is an independent project, not an official Sekonic product. It does not flash firmware or change the meter's region or radio configuration.

## Download and install

**[Download the v0.4.0 Mac app](https://github.com/guojiahetongxue/sekonic-l858d-language-switcher/releases/download/v0.4.0/L858D-Language-Switcher-v0.4.0-macOS.zip)** · [Latest release](https://github.com/guojiahetongxue/sekonic-l858d-language-switcher/releases/latest) · [Changelog](CHANGELOG.md)

- **macOS 13 or later.** One universal app supports Apple silicon and Intel Macs. A Windows version is not available.
- Firmware must be exactly **17, 18 or 19**, with reported region **0000, 0001, 0002 or 0003**, and current language English, Japanese or Chinese.
- Use a data-capable **Micro-B USB cable** and connect one powered-on meter.
- Extract the app ZIP, move `L858D Language Switcher.app` to Applications and open it. Using the app needs no Python, Homebrew, REA, Xcode, additional drivers or firmware files.

The app is ad-hoc signed and is not Apple-notarized. macOS may be unable to verify its developer. Follow “Opening on another Mac” in the [instructions](Docs/Read%20Me.txt) for the first launch.

## Use

1. Quit older Language Switchers, Sekonic Data Transfer Software (DTS), its updater and other meter tools.
2. Connect one meter, turn it on and click **Detect meter**.
3. Check the firmware, region, current language and verification notice. Once checks pass, click the target language.
4. Wait for saving to finish, keeping power and USB connected.
5. Disconnect USB, turn the meter off and on, and confirm the menu language. You can reconnect and detect it again.

The current-language button is disabled. Use the same steps to restore Japanese or English. Changing the app interface does not change the meter. If the result is uncertain, follow the displayed restart and detection instructions before attempting another write. Logs stay on your Mac; hide serial numbers before sharing them.

See the [instructions](Docs/Read%20Me.txt) for the full procedure and troubleshooting.

## Compatibility and physical testing

The tool accepts only the explicit versions and regions above. Device identity, current language and write capabilities must also pass checks. **Every physical observation comes from one Japanese-region (0002) unit**, not a sample of all regions or hardware batches.

| Firmware | Physical observations | Not established |
| --- | --- | --- |
| 17 | Chinese write, save, readback and restart persistence passed. English retained from 18 survived a restart and readback. | A fresh English write or a Japanese switch was not tested. |
| 18 | English write, save and readback passed. Existing Chinese survived a firmware transition and restart. | English restart persistence was not separately confirmed. Fresh Chinese and Japanese writes were not tested. |
| 19 | Chinese write, save, readback and restart persistence passed. English and Japanese save/readback passed. | Separate English and Japanese restart persistence was not confirmed. |

Regions **0000, 0001 and 0003** have firmware protocol analysis and offline tests only; no corresponding hardware has been tested. “Hardware records exist” in the app describes observations for a firmware/region configuration, not complete testing of every language operation. See [Compatibility and Verification](Docs/Compatibility%20and%20Verification.txt) for the evidence and limits.

**Other firmware versions stop without a language change.** For older firmware, quit this app and use [Sekonic's official software](https://sekonic.com/downloads/) to check whether an upgrade to **19** is available. If it is, follow the official update and restart procedure, fully quit DTS and its updater, then return to detect the meter and switch language. DTS has separate system requirements. If the meter is newer than 19 or an official upgrade to 19 is unavailable, contact Sekonic and wait for support; do not downgrade on your own. Firmware 17, 18 and 19 need no upgrade just to switch language.

This project includes no firmware updater and distributes no Sekonic firmware, extracted fonts or private device records. Saving invokes the original settings-save function for the current settings block. This is not a full-device backup or calibration tool.

## Build and test

On a Mac with **Python 3** and **Apple Command Line Tools**, run from the project root:

```sh
python3 build.py
sh Tests/Core/run_tests.sh
"dist/L858D Language Switcher.app/Contents/MacOS/L858DSwitcher" --self-check
```

Outputs appear in `dist/`, including the universal app and ZIP packages. The build and these offline tests do not access a meter. Core tests use a simulated device with AddressSanitizer and UndefinedBehaviorSanitizer; they do not replace physical validation. The compiled app needs none of these developer tools at runtime.

## Feedback and license

Open an [issue](https://github.com/guojiahetongxue/sekonic-l858d-language-switcher/issues/new/choose) with your macOS version, firmware, region code, steps to reproduce and redacted logs. **Do not upload real serial numbers or complete device firmware.**

Project code is available under the [MIT License](LICENSE).
