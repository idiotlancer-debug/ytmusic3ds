# YT Music 3DS

Reproductor de YouTube Music para Nintendo 3DS (homebrew), pensado para la **Old 3DS** (el primer modelo).

## Cómo funciona

La 3DS no puede hablar con YouTube directamente (TLS moderno, firmas en JavaScript, formatos Opus/AAC). Por eso hay dos piezas:

```
3DS (.3dsx)  --HTTP/Wi-Fi-->  server.py (PC o Raspberry Pi)  -->  YouTube Music
  UI + minimp3 + ndsp           búsqueda + yt-dlp + ffmpeg
```

- El servidor busca con `ytmusicapi` y transcodifica el audio a **MP3 CBR 128 kbps, estéreo, 44,1 kHz** en streaming.
- La 3DS descarga por HTTP a un buffer circular de 512 KB, decodifica con `minimp3` y reproduce con el DSP (`ndsp`).

Optimizaciones para Old 3DS: interfaz de texto (casi sin coste de CPU), red en el núcleo 1 (syscore) y decodificación en el núcleo 0, MP3 sin SIMD (la ARM11 no tiene NEON), pre-buffer de ~3 s, y pantalla superior apagable (tecla Y) para ahorrar batería.

## 1. Servidor (PC / Raspberry Pi)

Requisitos: Python 3.9+, `ffmpeg` en el PATH.

```bash
cd server
pip install -r requirements.txt
python server.py
```

Variables opcionales: `YTM3DS_PORT` (8000), `YTM3DS_BITRATE` (`128k`; usa `96k` si tu Wi-Fi va justo).
Si dejan de funcionar las reproducciones, actualiza yt-dlp (`pip install -U yt-dlp`); según su versión también puede pedir un runtime de JavaScript (Deno), consulta su wiki.

## 2. Compilar la app

1. Instala [devkitPro](https://devkitpro.org/wiki/Getting_Started) con los paquetes `3ds-dev`.
2. Descarga `minimp3.h` de <https://github.com/lieff/minimp3> (dominio público) y cópialo en `include/`.
3. Ejecuta `make`. Se genera `ytmusic3ds.3dsx`.
4. Copia el `.3dsx` a `sdmc:/3ds/` en la SD de la consola.

## 3. Requisitos en la consola

- Homebrew Launcher (CFW o similar).
- `sdmc:/3ds/dspfirm.cdc` (se obtiene con la app **DSP1**); sin él no hay audio.
- Wi-Fi 2,4 GHz con WPA2 (la 3DS no soporta 5 GHz ni WPA3), en la misma red que el servidor.

## Uso

La primera vez pide la dirección del servidor (por ejemplo `192.168.1.50:8000`) y la guarda en `sdmc:/3ds/ytmusic3ds/server.txt`.

| Botón | Acción |
|---|---|
| X | Buscar |
| ↑ / ↓ | Mover selección |
| A | Reproducir / pausa |
| B | Parar |
| L / R | Anterior / siguiente |
| ← / → | Volumen |
| Y | Pantalla superior on/off |
| SELECT | Cambiar servidor |
| START | Salir |

## Limitaciones

- No hay búsqueda dentro de la pista (es streaming) ni biblioteca/listas personales (requeriría autenticación en el servidor).
- La consola de texto solo pinta ASCII; el servidor quita acentos y caracteres no latinos de los títulos.
- Hay 2-5 s de espera al empezar cada canción (yt-dlp + pre-buffer).
- Código sin probar en hardware real: puede necesitar pequeños ajustes en la primera compilación.

Uso personal. Descargar o retransmitir contenido de YouTube puede ir contra sus condiciones de servicio.
