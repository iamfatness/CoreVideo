# Third-party notices

## FFmpeg (downloaded on request, not redistributed)

ISO recording uses FFmpeg (https://ffmpeg.org), licensed under the GNU General
Public License v3 for the builds CoreVideo uses. CoreVideo does **not** ship
FFmpeg. When the operator clicks *Download FFmpeg*, CoreVideo downloads a
pinned build directly from its upstream publisher and verifies its SHA-256:

| Platform | Publisher | Build |
|---|---|---|
| Windows x64 | gyan.dev (https://www.gyan.dev/ffmpeg/builds/) | 9.0.2 GPL essentials |
| macOS Apple Silicon | Martin Riedl (https://ffmpeg.martin-riedl.de) | 9.0.2 GPL |

The GPLv3 text is installed next to the binary as `LICENSE.txt`, together with
`provenance.txt` recording the exact source URL and hash. FFmpeg source code
is available from https://ffmpeg.org/download.html and from each publisher.
