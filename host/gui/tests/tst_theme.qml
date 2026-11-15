import QtQuick
import QtTest
import DriveLink.Style

TestCase {
    id: testCase
    name: "Theme"
    width: 400
    height: 400
    visible: true
    when: windowShown

    Component {
        id: textComponent
        Text { text: "0123456789" }
    }
    Component {
        id: iconComponent
        DLIcon {}
    }

    function cleanup() {
        Theme.dark = true
    }

    function test_style_installed() {
        verify(styleInstalled, "bundled fonts must load")
    }

    function test_dark_and_light_colours() {
        Theme.dark = true
        compare(Theme.bgApp.toString(), "#0b0d10")
        compare(Theme.accentDefault.toString(), "#3b7be8")
        compare(Theme.disarmBg.toString(), "#e0443b")
        Theme.dark = false
        compare(Theme.bgApp.toString(), "#f4f6f8")
        compare(Theme.accentDefault.toString(), "#2a64c9")
        compare(Theme.disarmBg.toString(), "#c2302a")
        // Text on accent stays white in both modes.
        compare(Theme.textOnAccent.toString(), "#ffffff")
    }

    function test_scrim_is_translucent() {
        verify(Theme.bgScrim.a > 0.3 && Theme.bgScrim.a < 0.7)
        Theme.dark = false
        verify(Theme.bgScrim.a > 0.3 && Theme.bgScrim.a < 0.7)
    }

    function test_sizes() {
        compare(Theme.sizeControl, 32)
        compare(Theme.sizeStatusbar, 56)
        compare(Theme.sizeNavrail, 72)
        compare(Theme.sizeDisarm, 40)
        compare(Theme.borderWidth, 1)
        compare(Theme.borderFocusWidth, 2)
        compare(Theme.space4, 4)
        compare(Theme.radiusLg, 8)
    }

    function test_series_colours() {
        compare(Theme.seriesColors.length, 8)
        compare(Theme.seriesColors[0], Theme.series1)
        const seen = {}
        for (let i = 0; i < Theme.seriesColors.length; ++i) {
            const key = Theme.seriesColors[i].toString()
            verify(!seen[key], "series colours are distinct")
            seen[key] = true
        }
    }

    function test_fonts_resolve() {
        const ui = createTemporaryObject(textComponent, testCase, { font: Theme.fontBodyDefault })
        compare(ui.fontInfo.family, "Inter")
        compare(ui.fontInfo.pixelSize, 14)
        const strong = createTemporaryObject(textComponent, testCase, { font: Theme.fontTitleSection })
        compare(strong.fontInfo.family, "Inter")
        compare(strong.fontInfo.weight, Font.DemiBold)
        const medium = createTemporaryObject(textComponent, testCase, { font: Theme.fontBodyStrong })
        compare(medium.fontInfo.weight, Font.Medium)
        const mono = createTemporaryObject(textComponent, testCase, { font: Theme.fontMonoDefault })
        compare(mono.fontInfo.family, "JetBrains Mono")
        compare(mono.fontInfo.pixelSize, 13)
    }

    function test_tabular_numerals_on_numeric_styles() {
        compare(Theme.fontBodyNumeric.features["tnum"], 1)
        compare(Theme.fontDisplayValue.features["tnum"], 1)
        verify(Theme.fontBodyDefault.features["tnum"] === undefined)
    }

    function test_label_caps_letter_spacing() {
        // Qt quantises letter spacing to 1/64 px.
        fuzzyCompare(Theme.fontLabelCaps.letterSpacing, 0.66, 0.016)
    }

    function test_every_bundled_icon_loads() {
        compare(bundledIcons.length, 67)
        for (let i = 0; i < bundledIcons.length; ++i) {
            const icon = createTemporaryObject(iconComponent, testCase, { name: bundledIcons[i] })
            compare(icon.status, Image.Ready, bundledIcons[i])
            compare(icon.width, 16)
        }
    }

    function test_icon_follows_colour() {
        const icon = createTemporaryObject(iconComponent, testCase,
                                         { name: "power", color: Qt.binding(() => Theme.stateArmed) })
        verify(icon.source.toString().endsWith("/power/f5b544"), icon.source)
        Theme.dark = false
        verify(icon.source.toString().endsWith("/power/b97a0e"), icon.source)
        compare(icon.status, Image.Ready)
    }
}
