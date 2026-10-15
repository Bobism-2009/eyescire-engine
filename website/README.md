# Eyescire website

Marketing site for Eyescire Engine (Godot-style structure, Eyescire brand).

## Preview

Open `index.html` in a browser, or from the repo root:

```cmd
start website\index.html
```

For a local server (optional):

```cmd
cd website
python -m http.server 8080
```

Then visit `http://localhost:8080`.

## Contents

- `index.html` — home page (platform download selector)
- `docs.html` — documentation
- `styles.css` — layout and theme
- `main.js` — nav, platform tabs, scroll reveal, hero motion
- `assets/logo.png` — brand mark
- `assets/hero.jpg` — full-bleed hero image
- `downloads/*.7z` — Windows installer and source archives

Rebuild download archives from the repo root with:

```cmd
build.cmd
release.cmd
installer.cmd
pack-downloads.cmd
```
