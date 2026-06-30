#!/usr/bin/env bash
# build_vitis.sh — Detecta Vitis/Vivado 2024.1, crea la plataforma BSP y compila pong_app.
#
# Uso (desde la raíz del repo o cualquier directorio):
#   bash scripts/build_vitis.sh
#
# El workspace se crea en <repo_root>/../pong_workspace por defecto.
# Para usar otra ruta:
#   WORKSPACE=/ruta/workspace bash scripts/build_vitis.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE="${WORKSPACE:-$(cd "$REPO_ROOT/.." && pwd)/pong_workspace}"

# ── Detectar Vitis (xsct) ─────────────────────────────────────────────────────
find_vitis_root() {
    if command -v xsct &>/dev/null; then
        local bin_dir
        bin_dir="$(dirname "$(command -v xsct)")"
        echo "$(cd "$bin_dir/.." && pwd)"
        return 0
    fi
    if [[ -n "${VITIS_ROOT:-}" && -f "$VITIS_ROOT/settings64.sh" ]]; then
        echo "$VITIS_ROOT"; return 0
    fi
    local fixed=(
        "/tools/Xilinx2/Vitis/2024.1"
        "/tools/Xilinx/Vitis/2024.1"
        "/opt/Xilinx/Vitis/2024.1"
        "/opt/xilinx/Vitis/2024.1"
        "$HOME/Xilinx/Vitis/2024.1"
    )
    for r in "${fixed[@]}"; do
        [[ -f "$r/settings64.sh" ]] && echo "$r" && return 0
    done
    for mount_base in /media /mnt /run/media; do
        [[ -d "$mount_base" ]] || continue
        local found
        found="$(find "$mount_base" -maxdepth 7 -name "settings64.sh" \
                     -path "*/Vitis/*" 2>/dev/null | head -1)"
        [[ -n "$found" ]] && echo "$(dirname "$found")" && return 0
    done
    echo ""; return 1
}

VITIS_ROOT="$(find_vitis_root || true)"

if [[ -z "$VITIS_ROOT" ]]; then
    echo ""
    echo "ERROR: No se encontró Vitis 2024.1."
    echo ""
    echo "  1) Carga el entorno manualmente:"
    echo "       source /ruta/a/Vitis/2024.1/settings64.sh"
    echo "       bash scripts/build_vitis.sh"
    echo "  2) O exporta la raíz:"
    echo "       export VITIS_ROOT=/ruta/a/Vitis/2024.1"
    echo "       bash scripts/build_vitis.sh"
    echo ""
    exit 1
fi

if ! command -v xsct &>/dev/null; then
    source "$VITIS_ROOT/settings64.sh"
fi

XSA="$REPO_ROOT/top_pong_project.xsa"
if [[ ! -f "$XSA" ]]; then
    echo "ERROR: XSA no encontrado en $XSA"
    echo "       Genera primero el bitstream con:  bash scripts/build_all.sh"
    exit 1
fi

echo "INFO: Vitis     : $VITIS_ROOT"
echo "INFO: Repo      : $REPO_ROOT"
echo "INFO: Workspace : $WORKSPACE"
echo "INFO: XSA       : $XSA"
echo ""

mkdir -p "$REPO_ROOT/logs"
xsct "$SCRIPT_DIR/create_vitis_app.tcl" "$REPO_ROOT" "$WORKSPACE" \
     2>&1 | tee "$REPO_ROOT/logs/build_vitis.log"

ELF="$WORKSPACE/pong_app/Debug/pong_app.elf"
if [[ -f "$ELF" ]]; then
    echo ""
    echo "DONE: ELF generado en $ELF"
else
    echo ""
    echo "WARN: El ELF no apareció en la ruta esperada ($ELF)."
    echo "      Revisa logs/build_vitis.log para más detalles."
    exit 1
fi
