#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Reproducible x86_64 AppImage builder for New Pokemon Snap Recomp (Linux).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${SNAP_LINUX_PORT_VERSION:-${SNAP_VERSION:-1.3.0-linux.1-rc1}}"
BUILD_DIR="${SNAP_BUILD_DIR:-${ROOT_DIR}/build/cmake-partb}"
OUT_DIR="${SNAP_DIST_DIR:-${ROOT_DIR}/dist}"
OUT_NAME="New-Pokemon-Snap-Linux-v${VERSION}-x86_64.AppImage"
OUT_FILE="${OUT_DIR}/${OUT_NAME}"
WORK_DIR="${ROOT_DIR}/build/appimage"
APP_DIR="${WORK_DIR}/New-Pokemon-Snap-Recomp.AppDir"
BIN_SRC="${BUILD_DIR}/snap_boot_linux"

if [[ -z "${SOURCE_DATE_EPOCH:-}" ]] && command -v git >/dev/null 2>&1 && git -C "${ROOT_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  SOURCE_DATE_EPOCH="$(git -C "${ROOT_DIR}" log -1 --format=%ct)"
  export SOURCE_DATE_EPOCH
fi

if [[ ! -x "${BIN_SRC}" ]]; then
  echo "error: missing executable ${BIN_SRC} (build snap_boot_linux first)" >&2
  exit 1
fi

APPIMAGETOOL="${APPIMAGETOOL:-${HOME}/.cache/snap-appimage/appimagetool-x86_64.AppImage}"
if [[ ! -x "${APPIMAGETOOL}" ]]; then
  if command -v appimagetool >/dev/null 2>&1; then
    APPIMAGETOOL="$(command -v appimagetool)"
  else
    echo "Downloading appimagetool-x86_64.AppImage to ${APPIMAGETOOL}..."
    mkdir -p "$(dirname "${APPIMAGETOOL}")"
    curl -fsSL -L "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" -o "${APPIMAGETOOL}"
    chmod +x "${APPIMAGETOOL}"
  fi
fi

rm -rf "${APP_DIR}"
mkdir -p "${APP_DIR}/usr/bin" "${APP_DIR}/usr/lib" \
         "${APP_DIR}/usr/share/applications" \
         "${APP_DIR}/usr/share/icons/hicolor/256x256/apps" \
         "${OUT_DIR}"

cp -a "${BIN_SRC}" "${APP_DIR}/usr/bin/snap_boot_linux"
chmod 755 "${APP_DIR}/usr/bin/snap_boot_linux"

# Bundle libSDL3.so.0 (all other runtime deps: libvulkan.so.1, glibc, libstdc++, NVIDIA/Mesa drivers come from the host).
SDL3_PATH="${SNAP_SDL3_PATH:-}"
if [[ -z "${SDL3_PATH}" ]]; then
  for cand in /usr/local/lib/libSDL3.so.0 /usr/lib64/libSDL3.so.0 /lib64/libSDL3.so.0 /usr/lib/x86_64-linux-gnu/libSDL3.so.0; do
    if [[ -e "${cand}" ]]; then
      SDL3_PATH="$(readlink -f "${cand}")"
      break
    fi
  done
fi
if [[ -z "${SDL3_PATH}" ]] && command -v podman >/dev/null 2>&1 && podman container exists snap-recomp-u22 2>/dev/null; then
  SDL3_TMP="${WORK_DIR}/libSDL3.so.0"
  if podman cp snap-recomp-u22:/usr/local/lib/libSDL3.so.0.4.20 "${SDL3_TMP}" 2>/dev/null; then
    SDL3_PATH="${SDL3_TMP}"
  fi
fi
if [[ -z "${SDL3_PATH}" ]] && command -v distrobox >/dev/null 2>&1; then
  SDL3_TMP="${WORK_DIR}/libSDL3.so.0"
  if distrobox enter snap-recomp-linux -- bash -c 'cp -L /usr/lib64/libSDL3.so.0 "$1"' _ "${SDL3_TMP}" 2>/dev/null; then
    SDL3_PATH="${SDL3_TMP}"
  fi
fi
if [[ -z "${SDL3_PATH}" || ! -f "${SDL3_PATH}" ]]; then
  echo "error: could not locate libSDL3.so.0 to bundle" >&2
  exit 1
fi
cp -L "${SDL3_PATH}" "${APP_DIR}/usr/lib/libSDL3.so.0"
chmod 755 "${APP_DIR}/usr/lib/libSDL3.so.0"

# Optionally bundle ShaderHarness if published locally.
HARNESS_DIR="${SNAP_SHADER_HARNESS_DIR:-}"
if [[ -z "${HARNESS_DIR}" ]]; then
  for cand_dir in \
    "${ROOT_DIR}/tools/shader_harness/bin/Release/net10.0/linux-x64/publish" \
    "${ROOT_DIR}/scripts/shader_harness/bin/Release/net10.0/linux-x64/publish" \
    "${ROOT_DIR}/tools/shader_harness/bin/Release/net10.0" \
    "${ROOT_DIR}/scripts/shader_harness/bin/Release/net10.0"; do
    if [[ -d "${cand_dir}" ]] && [[ -f "${cand_dir}/ShaderHarness" || -f "${cand_dir}/ShaderHarness.dll" ]]; then
      HARNESS_DIR="${cand_dir}"
      break
    fi
  done
fi
if [[ -n "${HARNESS_DIR}" && -d "${HARNESS_DIR}" ]] && [[ -f "${HARNESS_DIR}/ShaderHarness" || -f "${HARNESS_DIR}/ShaderHarness.dll" ]]; then
  mkdir -p "${APP_DIR}/usr/bin/shader_harness"
  cp -a "${HARNESS_DIR}/." "${APP_DIR}/usr/bin/shader_harness/"
  if [[ -f "${APP_DIR}/usr/bin/shader_harness/ShaderHarness" ]]; then
    chmod 755 "${APP_DIR}/usr/bin/shader_harness/ShaderHarness"
  fi
fi

# Generate a clean 256x256 icon without proprietary game assets.
ICON_PNG="${APP_DIR}/new-pokemon-snap-recomp.png"
if ! SNAP_ICON_SRC=none python3 "${ROOT_DIR}/host/make_icon.py" >/dev/null 2>&1 || [[ ! -f "${ROOT_DIR}/host/snap_icon.png" ]]; then
  if command -v magick >/dev/null 2>&1; then
    magick -size 256x256 xc:'#1b263b' -fill '#e0a458' -draw 'circle 128,128 128,44' "${ICON_PNG}"
  elif command -v convert >/dev/null 2>&1; then
    convert -size 256x256 xc:'#1b263b' -fill '#e0a458' -draw 'circle 128,128 128,44' "${ICON_PNG}"
  else
    echo "error: could not generate AppImage icon" >&2
    exit 1
  fi
else
  if command -v magick >/dev/null 2>&1; then
    magick "${ROOT_DIR}/host/snap_icon.png" -resize 256x256 "${ICON_PNG}"
  else
    cp -a "${ROOT_DIR}/host/snap_icon.png" "${ICON_PNG}"
  fi
fi
cp -a "${ICON_PNG}" "${APP_DIR}/usr/share/icons/hicolor/256x256/apps/new-pokemon-snap-recomp.png"
cp -a "${ICON_PNG}" "${APP_DIR}/.DirIcon"

cat > "${APP_DIR}/new-pokemon-snap-recomp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=New Pokemon Snap Recomp
Comment=Unofficial Linux port of New Pokemon Snap Recomp
Exec=snap_boot_linux
Icon=new-pokemon-snap-recomp
Categories=Game;
Terminal=false
X-AppImage-Version=${VERSION}
EOF
cp -a "${APP_DIR}/new-pokemon-snap-recomp.desktop" "${APP_DIR}/usr/share/applications/new-pokemon-snap-recomp.desktop"

cat > "${APP_DIR}/AppRun" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="${HERE}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export NVN_CACHE_DIR="${NVN_CACHE_DIR:-${XDG_CACHE_HOME:-${HOME}/.cache}/SnapRecomp/nvn_cache}"
export SNAP_CUTSCENE_ULTRAWIDE="${SNAP_CUTSCENE_ULTRAWIDE:-1}"
exec "${HERE}/usr/bin/snap_boot_linux" "$@"
EOF
chmod 755 "${APP_DIR}/AppRun"

rm -f "${OUT_FILE}" "${OUT_FILE}.sha256"
mkdir -p "${WORK_DIR}/tmp"
TMPDIR="${WORK_DIR}/tmp" APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "${APPIMAGETOOL}" --no-appstream "${APP_DIR}" "${OUT_FILE}"
chmod 755 "${OUT_FILE}"

(cd "${OUT_DIR}" && sha256sum "${OUT_NAME}" > "${OUT_NAME}.sha256")

echo "Built AppImage: ${OUT_FILE}"
ls -lh "${OUT_FILE}" "${OUT_FILE}.sha256"
cat "${OUT_FILE}.sha256"
