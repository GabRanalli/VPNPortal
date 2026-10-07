# Making a theme for VPN Portal

*[Leer en español](THEMES.es.md)*

A theme is a single `.css` file. VPN Portal is built with GTK 4 and
libadwaita, which draw everything using **CSS variables**, so most themes
just change a few colors. You can also add gradients, shadows and the like.

## Quick start

1. Copy the template [`example-theme.css`](example-theme.css) and give it
   a new file name, e.g. `my-theme.css`.
2. Change the details in the first comment and the colors.
3. In VPN Portal: **☰ → Preferences → Your themes → Add theme…** and pick
   your file. It gets copied to `~/.config/vpnportal/themes/` and applied.
4. Keep editing **the copy in that folder** (the 📁 button opens it): every
   time you save, the app reloads the theme. If there is a CSS error, the app
   tells you, and the line is in the full log.

Any `.css` file you drop in `~/.config/vpnportal/themes/` shows up in
Preferences too.

## The first comment: the theme's details

```css
/* VPN Portal theme
 * name: Sunset
 * description: Purples and oranges, at night
 * base: dark
 * swatch: #2b1838 #ff8a3d
 */
```

| Key           | Meaning                                                          |
|---------------|------------------------------------------------------------------|
| `name`        | Name shown in Preferences (default: the file name)               |
| `description` | Short line under the name                                        |
| `base`        | `light`, `dark` or `system`: libadwaita's base style             |
| `swatch`      | One or two colors for the little preview square                  |

Pick the `base` that matches your colors: with `dark`, everything you don't
change (icons, text, scrollbars...) is drawn for a dark background.

## The color variables

Set them inside `:root { ... }`. The most useful ones:

| Variable                    | What it colors                                     |
|-----------------------------|----------------------------------------------------|
| `--accent-bg-color`         | Main buttons ("Connect"), checked radios           |
| `--accent-fg-color`         | Text on top of the accent color                    |
| `--accent-color`            | Accent used as text color (links...)               |
| `--window-bg-color` / `-fg` | Window background / text                           |
| `--view-bg-color` / `-fg`   | Lists and text views                               |
| `--headerbar-bg-color` / `-fg` | Title bar                                       |
| `--card-bg-color` / `-fg`   | Cards (the "Connections" list...)                  |
| `--dialog-bg-color`         | Dialogs (edit VPN, Preferences...)                 |
| `--popover-bg-color`        | Menus                                              |
| `--destructive-bg-color`    | Danger buttons ("Disconnect", "Delete")            |
| `--border-color`            | Borders and separators                             |

On Ubuntu, libadwaita is patched with Yaru's style and also uses
`--yaru-accent-bg-color` and `--yaru-accent-color`: set them to the same
values as the accent ones.

The full list is in the
[libadwaita documentation](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/main/css-variables.html).

## Going further

Anything GTK's CSS supports works: gradients, `box-shadow`, borders,
`text-shadow`... Useful selectors:

| Selector                    | What it is                                        |
|-----------------------------|---------------------------------------------------|
| `window.vpnportal-main`     | The main window (only it, not the dialogs)        |
| `headerbar`                 | The title bar                                     |
| `.card`, `.boxed-list`      | Cards                                             |
| `button.suggested-action`   | Main buttons ("Connect", "Save")                  |
| `button.destructive-action` | Danger buttons ("Disconnect", "Delete")           |
| `.heading`                  | Card titles ("Connections", "Activity")           |

The built-in themes are good examples, especially
[Frutiger Aero](../data/themes/aero.css) and
[Neon blue](../data/themes/neon.css).

A theme can only change how the app looks: CSS can't run code, so installing
someone else's theme is safe.
