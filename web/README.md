# HDR Bridge Web

The browser application is an independent static frontend for GitHub Pages.
Conversion runs locally in a Web Worker with codec-specific WASM modules; no
image is uploaded.

`reference-ui/fileconverter.html` is the visual reference. The production UI
keeps its dark Fileconverter layout, warm gradient heading, orange selection
state, compact option boxes and native-output preview behavior.

The Web application processes one file at a time. It shares conversion
semantics with the native core but does not depend on the Windows desktop UI or
WIC. Unsupported browser codec paths are reported explicitly.
