# Cómo hacer un tema para VPN Portal

*[Read in English](THEMES.md)*

Un tema es un solo fichero `.css`. VPN Portal está hecha con GTK 4 y
libadwaita, que lo pintan todo con **variables CSS**, así que la mayoría de
temas solo cambian unos cuantos colores. También puedes añadir degradados,
sombras y demás.

## Para empezar

1. Copia la plantilla [`example-theme.css`](example-theme.css) con otro
   nombre, por ejemplo `mi-tema.css`.
2. Cambia los datos del primer comentario y los colores.
3. En VPN Portal: **☰ → Preferencias → Temas propios → Añadir tema…** y
   elige tu fichero. Se copia a `~/.config/vpnportal/themes/` y se aplica.
4. Sigue editando **la copia de esa carpeta** (el botón 📁 la abre): cada vez
   que guardas, la app recarga el tema. Si hay un error de CSS, la app te
   avisa, y la línea está en el registro completo.

Cualquier `.css` que dejes en `~/.config/vpnportal/themes/` también aparece
en Preferencias.

## El primer comentario: los datos del tema

```css
/* VPN Portal theme
 * name: Atardecer
 * description: Morados y naranjas, de noche
 * base: dark
 * swatch: #2b1838 #ff8a3d
 */
```

| Clave         | Qué es                                                           |
|---------------|------------------------------------------------------------------|
| `name`        | El nombre en Preferencias (si no, el del fichero)                |
| `description` | La línea corta bajo el nombre                                    |
| `base`        | `light`, `dark` o `system`: el estilo base de libadwaita         |
| `swatch`      | Uno o dos colores para el cuadrito de muestra                    |

Elige el `base` que pegue con tus colores: con `dark`, todo lo que no
cambies (iconos, texto, barras de scroll...) se pinta para fondo oscuro.

## Las variables de color

Van dentro de `:root { ... }`. Las más útiles:

| Variable                    | Qué colorea                                        |
|-----------------------------|----------------------------------------------------|
| `--accent-bg-color`         | Botones principales ("Conectar"), radios marcados  |
| `--accent-fg-color`         | El texto encima del color de acento                |
| `--accent-color`            | El acento como color de texto (enlaces...)         |
| `--window-bg-color` / `-fg` | Fondo / texto de la ventana                        |
| `--view-bg-color` / `-fg`   | Listas y vistas de texto                           |
| `--headerbar-bg-color` / `-fg` | La barra de título                              |
| `--card-bg-color` / `-fg`   | Tarjetas (la lista de "Conexiones"...)             |
| `--dialog-bg-color`         | Diálogos (editar VPN, Preferencias...)             |
| `--popover-bg-color`        | Menús                                              |
| `--destructive-bg-color`    | Botones de peligro ("Desconectar", "Eliminar")     |
| `--border-color`            | Bordes y separadores                               |

En Ubuntu, libadwaita lleva un parche con el estilo de Yaru que también usa
`--yaru-accent-bg-color` y `--yaru-accent-color`: ponles los mismos valores
que a las de acento.

La lista completa está en la
[documentación de libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/main/css-variables.html).

## Más allá

Funciona todo lo que admite el CSS de GTK: degradados, `box-shadow`, bordes,
`text-shadow`... Selectores útiles:

| Selector                    | Qué es                                            |
|-----------------------------|---------------------------------------------------|
| `window.vpnportal-main`     | La ventana principal (solo ella, no los diálogos) |
| `headerbar`                 | La barra de título                                |
| `.card`, `.boxed-list`      | Tarjetas                                          |
| `button.suggested-action`   | Botones principales ("Conectar", "Guardar")       |
| `button.destructive-action` | Botones de peligro ("Desconectar", "Eliminar")    |
| `.heading`                  | Títulos de tarjeta ("Conexiones", "Actividad")    |

Los temas de serie son buenos ejemplos, sobre todo
[Frutiger Aero](../data/themes/aero.css) y
[Azul neón](../data/themes/neon.css).

Un tema solo puede cambiar el aspecto de la app: el CSS no puede ejecutar
código, así que instalar el tema de otra persona es seguro.
