# msdisp-portrait

Substituto do WinUsbDisplay (MacroSilicon) para adaptadores USB→HDMI **MS912x** (`USB\VID_534D&PID_6021`), com **modo retrato de verdade**, conversão na GPU e latência mínima.

Funciona sem driver novo: o controle do chip passa pela interface HID nativa do Windows e a imagem pelo `libusb0` que o pacote "USM USB Display" (V2.0.1.7.3) já instalou.

## Como funciona

```
Monitor virtual Parsec (1080x1920, retrato nativo)
  → captura DXGI Desktop Duplication (com cursor)
  → GPU: gira 90°, converte BGRA→UYVY e detecta os tiles que mudaram
  → USB: um retângulo envolvente por envio + pacote vazio (ZLP), repetido nos 2 buffers do chip
  → MS912x → HDMI 1920x1080@60 → monitor girado
```

O pipeline é serializado: espera o envio USB terminar e só então captura o quadro mais novo. Assim nunca há fila (3 quadros cheios na fila davam ~530 ms de atraso) e nenhuma área alterada se perde.

## Instalação (feita em 17/10/2026)

- Executável: `%LOCALAPPDATA%\Programs\msdisp\msdisp.exe` (estático, sem DLLs extras).
- Início automático: `HKCU\...\Run\msdisp-portrait`. Também dá para ligar e desligar pelo menu da bandeja.
- Para atualizar: `msdisp.exe --quit`, copie o novo `build\msdisp.exe` por cima e abra de novo.

## Uso

1. Rode `build\msdisp.exe`. Aparece um ícone na bandeja, perto do relógio.
   - Ele para o WinUsbDisplay se estiver rodando, porque os dois não podem usar o adaptador ao mesmo tempo.
   - Cria o monitor virtual em retrato no lugar do monitor fantasma "MS Idd" do fabricante, à esquerda da tela principal.
2. Gire o monitor físico. Se a imagem ficar de cabeça para baixo, use **Rotação → 270°** no menu da bandeja.
3. Menu da bandeja:
   - Pausar/Retomar.
   - Rotação: 0/90/180/270.
   - Resolução: 1920x1080 ou 1280x720 (o 720p tem o dobro de fps, mas menos nitidez).
   - Mostrar métricas na tela.
   - Iniciar com o Windows.
   - **Área de trabalho → Salvar / Restaurar organização dos ícones**: guarda a posição de cada ícone (relativa ao monitor principal) em `%APPDATA%\msdisp\desktop-layout.txt` e devolve tudo ao lugar quando o Windows embaralhar os ícones (isso acontece quando o monitor virtual aparece, some ou muda de tamanho). Ícones novos vão para a esquerda (programas) ou para a direita (pastas).
   - **Voltar ao driver original**: para o msdisp e religa o WinUsbDisplay.
   - Abrir log e Abrir configuração.
4. Configuração: `%APPDATA%\msdisp\msdisp.ini`. Log: `%APPDATA%\msdisp\msdisp.log` (métricas a cada 30 s).

Pré-requisitos já presentes nesta máquina: Parsec Virtual Display Driver 0.45, com resoluções customizadas em `HKLM\SOFTWARE\Parsec\vdd`: slot 0 = 1080x1920 e slot 1 = 720x1280.

## Desempenho medido (MS912C, USB 2.0, Ryzen 5 5500U)

| Cenário | 1080p | 720p |
|---|---|---|
| Tela inteira mudando (limite do USB 2.0) | 7,6 fps / 32 MB/s | 15 fps |
| Arrastando uma janela de 600x700 | ~15,5 fps, ~32 ms | ~16 fps, ~28 ms |
| Cursor / mudanças pequenas | ~60 atualizações/s, ~2 ms | idem |
| Conversão na GPU por quadro | < 2 ms | < 2 ms |

O limite real é a banda do USB 2.0, porque o chip não tem compressão. O ganho vem de enviar só o que mudou e de não haver fila.

## Build

MinGW-w64 GCC + CMake + Ninja (não precisa de Visual Studio):

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

Ferramentas de diagnóstico:

| Ferramenta | O que faz |
|---|---|
| `msdisp-probe info` | Lê chip, HPD e EDID pelo HID (seguro com o WinUsbDisplay rodando) |
| `msdisp-probe bars\|bench\|trail` | Testes direto no chip; precisa do adaptador livre |
| `portrait-run` | Pipeline sem interface gráfica |
| `verify` | Compara captura + GPU com uma referência na CPU, sem USB |
| `vddtest` | Testa o monitor virtual Parsec |
| `gpubench` | Mede o tempo da conversão na GPU |

Variáveis de ambiente para experimentos: `MSDISP_MULTI_RECT=1`, `MSDISP_NBUF=1..4`, `MSDISP_ZLP=0`.

## O que aprendemos sobre o chip

Confirmado no hardware em 17/10/2026:

- Chip ID `b7 16 0a` = MS912C. Controle por feature report HID de 8 bytes; imagem por bulk no EP 0x04.
- **O chip alterna entre 2 framebuffers a cada envio.** Cada área alterada precisa ser enviada aos dois, senão aparecem rastros e pontos verdes (memória não inicializada, YUV 0,0,0).
- **Só um retângulo por envio é aplicado de forma confiável.** Por isso usamos o retângulo envolvente, como o driver oficial.
- O registrador `0xD003` (troca de frame no MS9132) não serve para o MS912x: fica sempre em 178.

## Licença

GPL-2.0, porque o protocolo foi derivado do código Linux da MacroSilicon e de [rhgndf/ms912x](https://github.com/rhgndf/ms912x).
