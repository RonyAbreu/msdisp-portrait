# msdisp-portrait — Plano de execução (Opção 3)

Substituto do WinUsbDisplay para o adaptador MacroSilicon **MS9120 (`USB\VID_534D&PID_6021`)**, com **modo retrato nativo**, pipeline na GPU e latência mínima.

## 1. O que já foi validado (sem tocar no hardware)

| Item | Resultado |
|---|---|
| Controle do chip | Interface `MI_00` = HID vendor (UsagePage `0xFF00`), **feature report de 9 bytes** (1 de ID + 8). Bate com o protocolo (`SET_REPORT/GET_REPORT`, `wValue=0x0300`). Dá para usar `HidD_SetFeature/GetFeature` nativos do Windows, **sem driver novo**. |
| Envio de imagem | Interface `MI_03` já está associada ao `libusb0.sys` (instalado pela V2.0.1.7.3). Bulk OUT no **EP 0x04**. `libusb0.dll` está em `System32`. |
| Protocolo | Duas referências GPL-2.0: código oficial MacroSilicon `MS91xx_Linux_Drm_SourceCode_V3.0.3.13` (que trata `CHIP_ID_9120`) e [rhgndf/ms912x](https://github.com/rhgndf/ms912x) (que trata `534d:6021`). Ambas já baixadas para estudo. |
| Comandos-chave | `0xA6` grava 6 bytes (power `0x07`, resolução `0x01`, modo `0x02`, output `0x05`); `0xB5` lê registrador (EDID em `0xC000`, HPD em `0x0031`). Modo **1920x1080@60 = `0x81`**, 1280x720@60 = `0x4F`. |
| Quadro | Cabeçalho `FF 00` + x,y (12 bits cada) + w,h (12 bits cada), seguido de UYVY (2 B/px) e terminador `FF C0 00 00 00 00 00 00`. Permite **retângulos parciais**. |
| Monitor virtual | Parsec VDD 0.45 instalado; resoluções customizadas ficam em `HKLM\SOFTWARE\Parsec\vdd\N` (width/height/hz). |
| Toolchain | MinGW-w64 GCC 16.2 + CMake + Ninja, com headers D3D11/DXGI/HID. Os shaders são compilados em runtime com o `d3dcompiler_47.dll` do sistema. **Não precisa de Visual Studio.** |

## 2. Arquitetura

```
 Parsec VDD (monitor virtual 1080x1920 nativo, retrato)
        │  DXGI Desktop Duplication (dirty rects + move rects do Windows)
        ▼
 GPU (D3D11 compute shader, um único dispatch):
   rotação 90° → BGRA→UYVY (BT.601, coeficientes do rhgndf)
   → compara com o quadro anterior em tiles de 64x8 → bitmap de tiles sujos
        │  CopyResource → staging (só as faixas sujas)
        ▼
 CPU: junta tiles em retângulos → monta pacotes (cabeçalho + UYVY + terminador)
        │  libusb0 async: 2–3 bulk writes em voo, sempre o quadro mais novo (descarta os atrasados)
        ▼
 MS9120 → HDMI 1920x1080@60 → monitor físico girado
```

Controle do chip (power, modo, EDID, hotplug) via HID feature reports em `MI_00`.

Processo único `msdisp.exe` em segundo plano, com ícone na bandeja, iniciado no logon pelo Agendador de Tarefas.

## 3. Estrutura do repositório e contratos

```
msdisp-portrait/
  CMakeLists.txt
  include/msdisp/          ← contratos (Fase 0, escritos por mim, congelados antes das trilhas)
    device.h    IDevice: open(), power(bool), setMode(Mode), readEdid(), hotplug(), sendFrame(span<Rect>)
    capture.h   ICapture: start(outputName), acquire(timeout) -> Frame{texture, dirtyRects}
    gpu.h       IConverter: process(texture, rotation) -> DirtyTiles + mapped UYVY
    vdd.h       IVirtualDisplay: add(w,h,hz), keepAlive(), remove()
    config.h    Config (ini): rotation, mode, fpsCap, tileSize...
  src/usb/      Trilha A
  src/vdd/      Trilha B
  src/capture/  Trilha B
  src/gpu/      Trilha C
  src/app/      Trilha D
  tools/        msdisp-probe (barras de cor), bench
  tests/
```

Cada trilha mexe **só na sua pasta**. `include/` é congelado e qualquer mudança passa por mim.

## 4. Fases

### Fase 0 — Esqueleto e spikes de risco (eu, nesta sessão · ~0,5–1 dia)
1. `git init`, CMake, `include/msdisp/*.h` (contratos), mocks e CI local (build + testes).
2. **Spike 1, controle HID:** ler chip ID (`0xF000`), status HPD e EDID. Só leitura, seguro com o WinUsbDisplay rodando.
3. **Spike 2, bulk (precisa parar o WinUsbDisplay por alguns minutos):** power on, modo `0x81`, enviar um quadro de barras de cor. Testar **vários retângulos num mesmo bulk** e medir o throughput real.
4. **Spike 3:** DXGI Duplication funciona em saída de IDD (Parsec)? Se não funcionar, plano B é Windows.Graphics.Capture.
5. **Spike 4:** quando o WinUsbDisplay fecha, o monitor "MS Idd" some? Isso define o layout final.

**Gate:** só abro as trilhas paralelas se os spikes 1–3 passarem. Se algum falhar, volto com você antes.

### Fase 1 — Trilhas paralelas (4 sessões em worktrees próprios · ~1–3 dias cada)

| Trilha | Escopo | Feito quando | Hardware? |
|---|---|---|---|
| **A — USB/protocolo** (`src/usb`) | Loader dinâmico da `libusb0.dll` (API `usb_*`, incluindo `usb_submit_async`), HID feature I/O, init/power/modo/EDID/hotplug, envio assíncrono com fila de 2–3 buffers e descarte do quadro velho. Ferramenta `msdisp-probe`. | O probe mostra barras de cor e retângulos em movimento; o bench mede MB/s e fps; desconectar e reconectar funciona. | **Sim**, é a dona exclusiva do adaptador. |
| **B — Monitor virtual + captura** (`src/vdd`, `src/capture`) | Controle do Parsec VDD por IOCTL (adicionar display, keepalive a cada <1 s, remover) e resolução 1080x1920 no registro. Captura DXGI da saída certa, com dirty/move rects e recuperação de `ACCESS_LOST`. | Monitor virtual em retrato aparece e some de forma limpa; a captura entrega quadros e retângulos sujos corretos. | Não (só o Parsec VDD). |
| **C — GPU** (`src/gpu`) | Compute shader (rotação 0/90/180/270 + UYVY + diff por tile) e readback só das faixas sujas. Testes com texturas sintéticas comparando contra uma implementação de referência na CPU. | Bit-exato contra a referência (±1 de arredondamento); <2 ms por quadro 1080p na Radeon. | Não. |
| **D — App shell** (`src/app`) | Bandeja (pausar, rotação, modo 1080p/720p, sair), config.ini, log, autostart (Agendador), convivência com o WinUsbDisplay (parar e restaurar), overlay de métricas (fps, MB/s, latência). | Liga e desliga sozinho no logon; botão "voltar ao driver original" funciona. | Não. |

Regras para as sessões: cada uma no **seu worktree e branch**, mexendo só na sua pasta, sem alterar `include/`. Só a Trilha A toca no adaptador.

### Fase 2 — Integração (eu · ~1–2 dias)
Merge das 4 branches (com sua autorização), pipeline completo em `msdisp.exe` e teste de ponta a ponta em retrato.

### Fase 3 — Tuning de latência e qualidade (~1–2 dias)
- Medir a latência real (câmera do celular filmando um cronômetro nos dois monitores).
- Ajustar tamanho de tile, número de transferências em voo, coalescência de retângulos e limite de fps.
- Testar modo 720p (mais fps) contra 1080p (mais nitidez).
- Testar se o chip aceita **RGB888** (`PIXFMT 0x11`): mais qualidade de cor com menos fps.

## 5. Riscos

| Risco | Prob. | Mitigação |
|---|---|---|
| DXGI Duplication não pega a saída do IDD da Parsec | Média | Spike 3. Plano B: Windows.Graphics.Capture. Plano C: o virtual display open-source *VirtualDrivers VDD*. |
| WinUsbDisplay e nosso app disputam o adaptador | Alta | O app para o WinUsbDisplay e tira ele do autostart, com botão de restaurar. |
| Vários retângulos por bulk não funcionam | Média | Spike 2. Senão, um bulk por retângulo com coalescência agressiva. |
| Keepalive da Parsec VDD 0.45 diferente do documentado | Baixa | Referência open-source (nomi-san/parsec-vdd) cobre a 0.45. |
| Banda do USB 2.0 | Certa | Não tem solução em software: ~8–10 fps em tela cheia a 1080p. O ganho vem de dirty rects e de a latência ser menor. Modo 720p como opção. |
| Licença GPL-2.0 das referências | — | O app também será GPL-2.0, o que não é problema para uso pessoal. |

## 6. Segurança e reversibilidade
- Nenhum driver de kernel novo e nenhuma assinatura de código: tudo roda em modo usuário.
- Não altera firmware nem flash do chip. Lê a flash só para pegar timings customizados, nunca grava.
- Restaurar o estado atual é só sair do app e religar o WinUsbDisplay (botão na bandeja). Já existe ponto de restauração de 17/10/2026.

## 7. Estimativa
Fase 0 em ~1 dia; Fase 1 com 4 trilhas em paralelo em ~2–3 dias corridos; Fases 2 e 3 em ~2–3 dias. **Total: ~1 semana**, contra 2–3 semanas sequencial.
