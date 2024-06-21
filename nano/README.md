# Nano patches

Just a few patches for [nano](https://www.nano-editor.org/), currently all based on commit 86108570.

- `blueless.patch` turns all blue syntax into cyan
- `tab-align.patch` makes tabs automatically align to the previous line (does not effect tab with a marked region or inverse tab)
- `better-browser.patch`:
- - Makes the browser default to the directory of the open file instead of the directory nano was ran from
- - Allows directories to be opened (which puts you in the file browser)
- `readonly-view.patch`: Turns on view mode for unwritable files (so you don't edit something only to not be able to save)
