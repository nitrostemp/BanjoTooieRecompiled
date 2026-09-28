#!/usr/bin/env python3
"""Create this project's simple geometric UI icons; no upstream artwork input.

SPDX-License-Identifier: GPL-3.0-only
AI-assisted authored source. Paths/dimensions implement the frontend asset API.
"""
from pathlib import Path

ICONS = {
    "Arrow": '<path d="M7 16h18m-8-8 8 8-8 8"/>',
    "Caret": '<path d="m7 12 9 9 9-9"/>',
    "Cont": '<path d="M9 9h14l5 13-4 3-6-6h-4l-6 6-4-3Z"/><path d="M9 13v6m-3-3h6m9-3h1m2 3h1"/>',
    "Keyboard": '<rect x="3" y="8" width="26" height="17" rx="2"/><path d="M7 12h1m4 0h1m4 0h1m4 0h3M7 16h1m4 0h1m4 0h1m4 0h3M9 21h14"/>',
    "Plus": '<path d="M16 6v20M6 16h20"/>',
    "Port": '<rect x="7" y="4" width="18" height="24" rx="3"/><path d="M11 10h10m-10 6h10m-10 6h10"/>',
    "Question": '<circle cx="16" cy="16" r="13"/><path d="M11 11c0-6 11-6 11 0 0 5-6 4-6 9m0 4v1"/>',
    "Quit": '<path d="M15 5H5v22h10m2-18 8 7-8 7M11 16h14"/>',
    "RecordSpinner": '<path d="M16 4a12 12 0 1 1-12 12"/><path d="M4 6v7h7"/>',
    "Reset": '<path d="M6 12a11 11 0 1 1 1 11M5 5v9h9"/>',
    "Trash": '<path d="M6 9h20M12 5h8M9 10l1 17h12l1-17M14 14v9m4-9v9"/>',
    "X": '<path d="m7 7 18 18M25 7 7 25"/>',
    "VizMap/DPad": '<path fill="#fff" fill-opacity=".15" d="M11 2h10v9h9v10h-9v9H11v-9H2V11h9Z"/>',
    "VizMap/DPadArrow": '<path d="m7 20 9-10 9 10Z" fill="#fff"/>',
    "VizMap/Map": '<rect x="1" y="1" width="30" height="30" rx="3" fill="#fff" fill-opacity=".12"/>',
    "VizMap/Shield": '<path d="M5 5h22v11c0 7-11 13-11 13S5 23 5 16Z" fill="#fff" fill-opacity=".15"/>',
    "VizMap/Target": '<circle cx="16" cy="16" r="11"/><circle cx="16" cy="16" r="4"/><path d="M16 1v7m0 16v7M1 16h7m16 0h7"/>',
}
SIZES = {"VizMap/DPad": 192, "VizMap/DPadArrow": 60,
         "VizMap/Map": 136, "VizMap/Shield": 96, "VizMap/Target": 154}

def svg(body, width=32, height=32, view="0 0 32 32", stroke=2):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="{view}">\n'
            '<!-- Project-authored geometric icon; SPDX-License-Identifier: GPL-3.0-only -->\n'
            f'<g fill="none" stroke="#fff" stroke-width="{stroke}" stroke-linecap="round" stroke-linejoin="round">'
            f'{body}</g>\n</svg>\n')

def main():
    root = Path(__file__).resolve().parents[1] / "assets/icons"
    output = {name: svg(body, SIZES.get(name, 32), SIZES.get(name, 32), stroke=1 if name.startswith("VizMap/") else 2)
              for name, body in ICONS.items()}
    for name, size in (("ButtonLarge", 84), ("ButtonMedium", 76), ("ButtonSmall", 64)):
        output[f"VizMap/{name}"] = svg('<circle cx="16" cy="16" r="14" fill="#fff" fill-opacity=".18"/>', size, size, stroke=1)
    output["RecordBorder"] = svg('<circle cx="16" cy="16" r="14"/>', 40, 40)
    output["PlusKeyboard"] = svg('<path d="M6 5v10M1 10h10"/><rect x="16" y="3" width="30" height="15" rx="2"/><path d="M21 7h1m4 0h1m4 0h1m4 0h5M22 13h17"/>', 48, 20, "0 0 48 20", 1.5)
    for name, content in output.items():
        path = root / (name + ".svg")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="\n")
    print(f"Wrote {len(output)} project-authored SVG icons")

if __name__ == "__main__":
    main()
