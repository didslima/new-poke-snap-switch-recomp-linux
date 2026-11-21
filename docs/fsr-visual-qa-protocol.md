# Protocolo de Validação Visual: FSR 3.1 vs DLSS vs Native (Linux)

**AppImage de referência:** `dist/New-Pokemon-Snap-Linux-v1.3.0-linux.14-fsr3-lanczos-test-x86_64.AppImage`  
**SHA256:** `2513c8f9883ae331a2f4353ac8ae3037e65b62affa5777b411b42d5a76212b0b`

---

## 1. Configurações fixas entre os 3 modos

Para que a comparação seja justa, mantenha idênticos:

- **Resolução de saída:** `--fullscreen --width 5120 --height 1440` (ou `--windowed --width 2560 --height 1440`).
- **HDR10:** `SNAP_HDR=hdr10` (ou `SNAP_HDR=off` em ambos se testar SDR).
- **Ultrawide 32:9:** `SNAP_CUTSCENE_ULTRAWIDE=1`.
- **Frame cap e VSync:** `SNAP_FPS=240` e `SNAP_VSYNC=on` (ou `SNAP_VSYNC=off` em todos).
- **Isolamento de saves e janela:** `--data-dir /tmp/snap-qa-<modo>` e `SNAP_WINDOW_SAVE=0` (nunca altera `~/.local/share/SnapRecomp`).
- **Logs de diagnóstico:** `SNAP_UPSCALE_LOG=1` (ou `2` para `FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING` + logs por frame).

---

## 2. Comandos exatos para cada modo

Substitua `<ROMFS_DIR>` pelo diretório `romfs` (ex.: `"/home/dlima/Games/Pokemon Snap/romfs"`).  
*(Se o binário precisar dos caminhos explícitos de ELF/AOT, mantenha `--main`, `--sdk`, `--main-lib`, `--sdk-lib` idênticos nos 3 comandos.)*

### 2.1 FSR 3.1 Quality (`SNAP_UPSCALER=fsr`)

```bash
mkdir -p /tmp/snap-qa-fsr
env SNAP_UPSCALER=fsr SNAP_UPSCALE_QUALITY=quality \
    SNAP_HDR=hdr10 SNAP_FPS=240 SNAP_VSYNC=on \
    SNAP_CUTSCENE_ULTRAWIDE=1 SNAP_WINDOW_SAVE=0 \
    SNAP_UPSCALE_LOG=1 \
    ./dist/New-Pokemon-Snap-Linux-v1.3.0-linux.14-fsr3-lanczos-test-x86_64.AppImage \
    --play --allow-guest-exec --fullscreen --width 5120 --height 1440 \
    --romfs "/home/dlima/Games/Pokemon Snap/romfs" \
    --data-dir /tmp/snap-qa-fsr \
    2>&1 | tee /tmp/snap-qa-fsr/run.log
```

### 2.2 DLSS Quality (`SNAP_UPSCALER=dlss`)

```bash
mkdir -p /tmp/snap-qa-dlss
env SNAP_UPSCALER=dlss SNAP_UPSCALE_QUALITY=quality \
    SNAP_HDR=hdr10 SNAP_FPS=240 SNAP_VSYNC=on \
    SNAP_CUTSCENE_ULTRAWIDE=1 SNAP_WINDOW_SAVE=0 \
    SNAP_UPSCALE_LOG=1 \
    ./dist/New-Pokemon-Snap-Linux-v1.3.0-linux.14-fsr3-lanczos-test-x86_64.AppImage \
    --play --allow-guest-exec --fullscreen --width 5120 --height 1440 \
    --romfs "/home/dlima/Games/Pokemon Snap/romfs" \
    --data-dir /tmp/snap-qa-dlss \
    2>&1 | tee /tmp/snap-qa-dlss/run.log
```

### 2.3 Native Rendering (`SNAP_UPSCALER=off`)

```bash
mkdir -p /tmp/snap-qa-native
env SNAP_UPSCALER=off \
    SNAP_HDR=hdr10 SNAP_FPS=240 SNAP_VSYNC=on \
    SNAP_CUTSCENE_ULTRAWIDE=1 SNAP_WINDOW_SAVE=0 \
    SNAP_UPSCALE_LOG=1 \
    ./dist/New-Pokemon-Snap-Linux-v1.3.0-linux.14-fsr3-lanczos-test-x86_64.AppImage \
    --play --allow-guest-exec --fullscreen --width 5120 --height 1440 \
    --romfs "/home/dlima/Games/Pokemon Snap/romfs" \
    --data-dir /tmp/snap-qa-native \
    2>&1 | tee /tmp/snap-qa-native/run.log
```

*(Opcional: para alternar automaticamente entre DLSS e FSR quadro a quadro na mesma cena, use `SNAP_UPSCALE_COMPARE=1`.)*

---

## 3. Logs de confirmação e caminhos

- **Caminho dos logs:** `/tmp/snap-qa-fsr/run.log`, `/tmp/snap-qa-dlss/run.log`, `/tmp/snap-qa-native/run.log`.
- **Linhas-chave a verificar nos logs (`grep "\[upscale\]" /tmp/snap-qa-*/run.log`):**
  - **Modo ativo e fator:** `[upscale] active: FSR 3.1 Quality (scene = output / 1.500)` ou `DLSS Quality` (ou ausência de upscale ativo em `off`).
  - **Resolução interna e final:** `[upscale] fsr: FSR 3.1.4 native Linux Vulkan context <W_in>x<H_in> -> <W_out>x<H_out>, HDR colour, inverted depth` ou `[upscale] dlss: feature created <W_in>x<H_in> -> <W_out>x<H_out>, ...`.
  - **Estado do dispatch:** `[upscale] p<N> upscaled <W_in>x<H_in> -> <W_out>x<H_out> jitter (...)` e métricas periódicas `[upscale] p300 stats: ...`.
  - **Suspensão temporária esperada (fotos / Evaluation):** `lateActive=1` / `hold on`.

---

## 4. Captura de screenshots sem alterar saves

- O gravador nativo de PNG por `F12` (`nvn_shot.cpp` / `ShotWritePng`) é um stub no Linux (`nvn_optional_stubs.cpp`).
- Para capturar screenshots comparáveis sem tocar em `~/.local/share/SnapRecomp`:
  - Use a ferramenta do desktop Linux (ex.: KDE Spectacle via `Print` ou `spectacle -a -b -o /tmp/snap-qa-<modo>/shot_<cena>.png`).
  - As fotos do álbum do jogo e eventuais exportações ficam isoladas em `/tmp/snap-qa-<modo>/`.

---

## 5. Checklist de avaliação visual em gameplay

Avaliar cada cena nos 3 modos (`FSR Quality`, `DLSS Quality`, `Native`):

- [ ] **Pokémon em movimento rápido:** verificar *ghosting*, rastro temporal e estabilidade de silhueta.
- [ ] **Vegetação e folhas:** observar *shimmering*, serrilhado em movimento e estabilidade de *alpha test*.
- [ ] **Água e transparências:** checar artefatos de reprojeção sobre água, reflexos e partículas transparentes.
- [ ] **Movimento rápido de câmera:** observar *disocclusion* nas bordas da tela e recuperação após giros rápidos.
- [ ] **Cutscenes ultrawide 32:9:** confirmar ausência de cortes, distorção de FOV ou artefatos nas bordas laterais.
- [ ] **Photo Evaluation:** verificar nitidez das miniaturas/fotos, enquadramento 16:9 e ausência de flickering.
- [ ] **Research Level / Expedition Points:** verificar estabilidade do Professor Mirror, texto e composição 2D/3D pós-Evaluation.

---

## 6. Limitações que não podem ser verificadas automaticamente

- Qualidade perceptual subjetiva (*ghosting* fino, *shimmering*, *blur* em movimento, *disocclusion* em Pokémon rápidos).
- Estabilidade visual do Professor Mirror na tela de Research Level / Expedition Points (já conhecida no DLSS como issue de baixa prioridade, a observar comparativamente no FSR 3.1).
- Comportamento em GPUs AMD (Mesa RADV) e Intel (Mesa ANV), ainda não testadas em hardware real.
