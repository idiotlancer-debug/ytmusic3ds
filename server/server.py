#!/usr/bin/env python3
"""
Servidor puente para YT Music 3DS.

La 3DS no puede hablar con YouTube directamente (TLS moderno, descifrado de
firmas en JavaScript, formatos Opus/AAC...). Este servidor, que corre en tu PC
o en una Raspberry Pi dentro de tu red local, hace ese trabajo pesado:

  GET /search?q=texto      -> lista de canciones en texto plano (TSV)
  GET /stream/<videoId>.mp3 -> audio MP3 CBR estereo 44.1 kHz en streaming

Requisitos: Python 3.9+, ffmpeg en el PATH y `pip install -r requirements.txt`
"""
import os
import re
import subprocess
import sys
import unicodedata

from flask import Flask, Response, abort, request
from ytmusicapi import YTMusic

HOST = os.environ.get("YTM3DS_HOST", "0.0.0.0")
PORT = int(os.environ.get("YTM3DS_PORT", "8000"))
BITRATE = os.environ.get("YTM3DS_BITRATE", "128k")  # 96k / 128k / 192k
FFMPEG = os.environ.get("YTM3DS_FFMPEG", "ffmpeg")
MAX_RESULTS = 15

VIDEO_ID = re.compile(r"^[A-Za-z0-9_-]{11}$")

app = Flask(__name__)
ytm = YTMusic()  # sin autenticacion: solo busqueda publica


def ascii_clean(text, fallback="?"):
    """La consola de la 3DS solo pinta ASCII: quitamos acentos y simbolos raros."""
    text = unicodedata.normalize("NFKD", text or "").encode("ascii", "ignore").decode()
    text = re.sub(r"[\t\r\n]+", " ", text).strip()
    return text or fallback


@app.get("/")
def index():
    return Response("YT Music 3DS server OK\n", mimetype="text/plain")


@app.get("/search")
def search():
    query = request.args.get("q", "").strip()
    if not query:
        return Response("", mimetype="text/plain")
    try:
        results = ytm.search(query, filter="songs", limit=MAX_RESULTS)
    except Exception as exc:  # noqa: BLE001
        return Response(f"error: {exc}\n", status=502, mimetype="text/plain")

    lines = []
    for item in results:
        video_id = item.get("videoId")
        if not video_id:
            continue
        artists = ", ".join(a.get("name", "") for a in item.get("artists") or [])
        lines.append(
            "\t".join(
                [
                    video_id,
                    ascii_clean(item.get("title"), "(sin titulo)"),
                    ascii_clean(artists, "-"),
                    str(int(item.get("duration_seconds") or 0)),
                ]
            )
        )
        if len(lines) >= MAX_RESULTS:
            break
    return Response("\n".join(lines) + "\n", mimetype="text/plain")


@app.get("/stream/<video_id>.mp3")
def stream(video_id):
    if not VIDEO_ID.match(video_id):
        abort(400)

    ytdlp = subprocess.Popen(
        [
            sys.executable, "-m", "yt_dlp",
            "-q", "--no-warnings", "--no-playlist",
            "-f", "bestaudio[ext=webm]/bestaudio",
            "-o", "-",
            f"https://music.youtube.com/watch?v={video_id}",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    ffmpeg = subprocess.Popen(
        [
            FFMPEG, "-hide_banner", "-loglevel", "error",
            "-i", "pipe:0",
            "-vn", "-map_metadata", "-1",
            "-ac", "2", "-ar", "44100",
            "-codec:a", "libmp3lame", "-b:a", BITRATE,
            "-write_xing", "0", "-id3v2_version", "0",
            "-flush_packets", "1",
            "-f", "mp3", "pipe:1",
        ],
        stdin=ytdlp.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    ytdlp.stdout.close()

    def generate():
        try:
            while True:
                chunk = ffmpeg.stdout.read1(8192)
                if not chunk:
                    break
                yield chunk
        finally:  # la 3DS cerro la conexion o termino la cancion
            for proc in (ffmpeg, ytdlp):
                if proc.poll() is None:
                    proc.kill()
                proc.wait()

    return Response(generate(), mimetype="audio/mpeg", headers={"Cache-Control": "no-store"})


if __name__ == "__main__":
    print(f"Servidor en http://{HOST}:{PORT}  (bitrate {BITRATE})")
    app.run(host=HOST, port=PORT, threaded=True)
