# KernelP

Compressor de video local para Windows. Interface premium em WebView2, nucleo em C++
e compressao acelerada pela GPU via FFmpeg NVENC. Sem upload, sem anuncio, sem fila.

## Como funciona
- Janela nativa C++ (`src/main.cpp`) hospeda a interface WebView2.
- O nucleo (`src/engine.cpp`) detecta a GPU, le o video com ffprobe e conduz o
  ffmpeg calculando o bitrate alvo para atingir a reducao escolhida.
- Na sua maquina o encoder padrao e HEVC NVENC na RTX 4060. AV1 NVENC tambem
  disponivel. Sem GPU compativel cai para HEVC Quick Sync ou libx265 na CPU.

## Executavel unico
A interface e o FFmpeg ficam embutidos dentro do proprio `KernelP.exe` como
recursos (ver `src/app.rc`). Voce compartilha so o `.exe`, com cerca de 183 MB.

- A interface e servida direto da memoria, sem pasta ao lado.
- Na primeira abertura o app extrai o FFmpeg para `%LOCALAPPDATA%\KernelP\runtime`
  uma unica vez. Depois abre na hora.
- Se existir uma pasta `bin` ao lado do exe, ele usa essa em vez de extrair. Util
  durante o desenvolvimento.

## Requisitos
- Windows 10 ou 11 de 64 bits com WebView2 Runtime (ja vem no Windows 11)
- Nada mais para usar. Tudo vai dentro do executavel
- Visual Studio 2022 com ferramentas C++ apenas para compilar

## Compilar
As pastas `ui` e `bin` precisam existir na hora de compilar porque o `app.rc`
as embute. O `bin` traz `ffmpeg.exe`, `ffprobe.exe` e as DLLs do build compartilhado.

## Compilar
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
O executavel fica em `build/Release/KernelP.exe` com a pasta `ui` ao lado.

## Usar
1. Abra o KernelP.
2. Escolha ou arraste um video.
3. Ajuste quanto reduzir, o codec e a velocidade.
4. Clique em Comprimir. O resultado sai na mesma pasta com o sufixo `_KernelP.mp4`.

## Desempenho medido
94.6 MB para 9.77 MB, reducao de 89.7%, em 1.77 s num clipe 1080p de 20 s na RTX 4060.
