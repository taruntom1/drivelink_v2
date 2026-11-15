# Bundled fonts and icons

Third-party files compiled into the `DriveLink.Style` Qt resources. They are
committed unchanged so builds need no network. The installer (H2g) ships these
notices with the other third-party notices.

| Files | Upstream | Version | Licence |
|---|---|---|---|
| `fonts/Inter-Regular.ttf`, `Inter-Medium.ttf`, `Inter-SemiBold.ttf` (from `extras/ttf/`) | https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip | 4.1 | SIL OFL 1.1, `fonts/Inter-OFL.txt` |
| `fonts/JetBrainsMono-Regular.ttf`, `JetBrainsMono-Medium.ttf` (from `fonts/ttf/`) | https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip | 2.304 | SIL OFL 1.1, `fonts/JetBrainsMono-OFL.txt` |
| `icons/*.svg` (67 icons from `icons/`) | https://registry.npmjs.org/lucide-static/-/lucide-static-0.460.0.tgz | 0.460.0 | ISC, `icons/LICENSE` |

Archive SHA256:

```
9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e  Inter-4.1.zip
6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf  JetBrainsMono-2.304.zip
24ec818481591e6795c48cd5d8466d1632c02050aa317801c207f91de9747ae4  lucide-static-0.460.0.tgz
```

Font SHA256:

```
40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82  Inter-Regular.ttf
97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872  Inter-Medium.ttf
78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3  Inter-SemiBold.ttf
a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f  JetBrainsMono-Regular.ttf
31c92d01a8a08528b718a43addf0ad3df0af2ca4b7b3290a452f70f358e14d3d  JetBrainsMono-Medium.ttf
```

The icon set is exactly the `Icon/<name>` components on the Figma G2 page
(Lucide 0.460, stroke `currentColor`, recoloured by `DLIcon`). To add an icon,
add its component in Figma first, then copy the same-version SVG here and list
it in `host/gui/style/CMakeLists.txt`.
