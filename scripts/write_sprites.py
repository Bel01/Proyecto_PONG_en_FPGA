#!/usr/bin/env python3
"""
write_sprites.py — Convierte BMPs a 4bpp y los graba en la microSD del Pong.

Paleta FPGA (índice: RGB 8-bit):
  0=Negro  1=Blanco  2=Rojo    3=Azul    4=Amarillo  5=Verde
  6=Naranja 7=Gris medio 8=Gris oscuro 9=Magenta A=Cyan B=Amarillo claro
  C=Rojo oscuro D=Verde oscuro E=Azul oscuro F=Gris claro

Layout de la SD (sectores de 512 B):
  LBA 1 : header magic "PONG"
  LBA 2 : ball      8×8  px  4bpp = 32 B
  LBA 3 : paddle    8×60 px  4bpp = 240 B
  LBA 5 : logo      64×16 px 4bpp = 512 B
  LBA 6 : gameover  200×225 px 4bpp = 22500 B (44 sectores, LBA 6-49)
  LBA 50: mode_sel  200×112 px 4bpp = 11200 B (22 sectores, LBA 50-71)
  LBA 72: pause     200×80  px 4bpp = 8000  B (16 sectores, LBA 72-87)

Sprites fuente en assets/ del repositorio (BMP originales de Bel01 + pause_menu generado).

Uso:
  sudo python3 write_sprites.py /dev/sdb [--dry-run]
"""

import sys
import struct
import os
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow no está instalado. Ejecuta: pip install Pillow")
    sys.exit(1)

# ── Paleta FPGA 16 colores (R,G,B en 8 bits) ─────────────────────────────────
PALETTE = [
    (  0,   0,   0),  # 0  Negro
    (255, 255, 255),  # 1  Blanco
    (255,   0,   0),  # 2  Rojo
    (  0,   0, 255),  # 3  Azul
    (255, 255,   0),  # 4  Amarillo
    (  0, 255,   0),  # 5  Verde
    (255, 136,   0),  # 6  Naranja
    (136, 136, 136),  # 7  Gris medio
    ( 68,  68,  68),  # 8  Gris oscuro
    (255,   0, 255),  # 9  Magenta
    (  0, 255, 255),  # A  Cyan
    (255, 255, 170),  # B  Amarillo claro
    (170,   0,   0),  # C  Rojo oscuro
    (  0, 170,   0),  # D  Verde oscuro
    (  0,   0, 170),  # E  Azul oscuro
    (170, 170, 170),  # F  Gris claro
]

def nearest_color(r, g, b):
    """Devuelve el índice de paleta más cercano (distancia euclidiana en RGB)."""
    best, best_d = 0, float('inf')
    for i, (pr, pg, pb) in enumerate(PALETTE):
        d = (r - pr)**2 + (g - pg)**2 + (b - pb)**2
        if d < best_d:
            best_d, best = d, i
    return best

def img_to_4bpp(img, w, h, transparent_rgb=None, resample=Image.LANCZOS):
    """
    Redimensiona img a w×h y convierte a 4bpp (2 píxeles por byte, high nibble primero).
    Los píxeles con color exactamente igual a transparent_rgb se mapean al índice 0.
    Devuelve bytes de longitud w*h//2.
    """
    img = img.convert('RGB').resize((w, h), resample)
    pixels = list(img.getdata())
    data = bytearray()
    for i in range(0, w * h, 2):
        p0 = pixels[i]
        p1 = pixels[i + 1] if i + 1 < w * h else (0, 0, 0)
        if transparent_rgb and p0 == transparent_rgb:
            n0 = 0
        else:
            n0 = nearest_color(*p0)
        if transparent_rgb and p1 == transparent_rgb:
            n1 = 0
        else:
            n1 = nearest_color(*p1)
        data.append((n0 << 4) | n1)
    return bytes(data)

def write_sector(dev, lba, data):
    """Escribe data (exactamente 512 B) en el sector LBA de dev."""
    assert len(data) == 512, f"Sector debe ser 512 B, got {len(data)}"
    with open(dev, 'r+b') as f:
        f.seek(lba * 512)
        f.write(data)

def pad_to_sector(data):
    """Rellena con ceros hasta múltiplo de 512."""
    rem = len(data) % 512
    if rem:
        data += b'\x00' * (512 - rem)
    return data

def main():
    dry_run = '--dry-run' in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith('--')]

    if not args:
        print(__doc__)
        sys.exit(1)

    dev = args[0]
    if not dry_run and not os.path.exists(dev):
        print(f"ERROR: {dev} no existe")
        sys.exit(1)

    script_dir = Path(__file__).parent
    repo_root  = script_dir.parent
    asset_dir  = repo_root / 'assets'

    # ── Escribir magic "PONG" en LBA1 ────────────────────────────────────────
    if not dry_run:
        magic_sector = bytearray(512)
        magic_sector[0:4] = b'PONG'
        write_sector(dev, 1, bytes(magic_sector))
        print("INFO: Magic PONG escrito en LBA1.")

    written = []

    # ── Función auxiliar ──────────────────────────────────────────────────────
    def write_sprite(lba, raw, label):
        sector = pad_to_sector(bytearray(raw))[:512]
        if dry_run:
            print(f"  [DRY-RUN] {label}: {len(raw)} B: LBA{lba}")
        else:
            write_sector(dev, lba, bytes(sector))
            print(f"  OK: {label}: {len(raw)} B: LBA{lba}")
        written.append((lba, label))

    # ── Ball: 8×8 px: 32 B ──────────────────────────────────────────────────
    ball_img = Image.new('RGB', (8, 8), (255, 255, 255))  # blanco sólido
    ball_raw = img_to_4bpp(ball_img, 8, 8)
    write_sprite(2, ball_raw, "ball 8×8")

    # ── Paddle: 8×60 px ──────────────────────────────────────────────────────
    # Diseño: franja gris claro (F) en el centro con bordes blancos (1)
    pad_img = Image.new('RGB', (8, 60), (255, 255, 255))
    from PIL import ImageDraw
    draw = ImageDraw.Draw(pad_img)
    draw.rectangle([1, 1, 6, 58], fill=(170, 170, 170))  # gris claro interior
    pad_raw = img_to_4bpp(pad_img, 8, 60)
    write_sprite(3, pad_raw, "paddle 8×60")

    # ── Logo: 64×16 px desde title.bmp ───────────────────────────────────────
    logo_bmp = asset_dir / 'title.bmp'
    if logo_bmp.exists():
        logo_img = Image.open(logo_bmp)
        # Color key magenta (255,0,255): transparente (índice 0)
        logo_raw = img_to_4bpp(logo_img, 64, 16, transparent_rgb=(255, 0, 255),
                               resample=Image.NEAREST)
        print(f"  INFO: title.bmp: 64×16 px, {len(logo_raw)} B")
    else:
        # Fallback: texto "PONG" simple en blanco sobre negro
        print("  AVISO: title.bmp no encontrado, usando fallback blanco.")
        logo_img = Image.new('RGB', (64, 16), (0, 0, 0))
        draw = ImageDraw.Draw(logo_img)
        draw.rectangle([8, 4, 55, 11], fill=(255, 255, 255))
        logo_raw = img_to_4bpp(logo_img, 64, 16)
    write_sprite(5, logo_raw, "logo 64×16")

    # ── Gameover: 160×225 px desde gameover.bmp (firmware escala 3×: 480×675) ─
    # El sprite se almacena como 160×225 4bpp = 18000 B = 36 sectores (LBA6-41)
    go_bmp = asset_dir / 'gameover.bmp'
    if go_bmp.exists():
        go_img  = Image.open(go_bmp)
        go_raw  = img_to_4bpp(go_img, 200, 225, resample=Image.NEAREST)
        go_data = pad_to_sector(bytearray(go_raw))
        sectors = len(go_data) // 512
        if dry_run:
            print(f"  [DRY-RUN] gameover 200×225: {len(go_raw)} B: LBA6-{6+sectors-1}")
        else:
            with open(dev, 'r+b') as f:
                f.seek(6 * 512)
                f.write(go_data)
            print(f"  OK: gameover 200×225: {len(go_raw)} B: LBA6-{6+sectors-1} ({sectors} sectores)")
        written.append((6, f"gameover 200×225 ({sectors} sectores)"))
    else:
        print("  AVISO: gameover.bmp no encontrado, se omite.")

    # ── Mode-select: 160×90 px desde mode_select.bmp ─────────────────────────
    # LBA 42-56 (15 sectores). Fondo negro: índice 0 (transparente en blit).
    ms_bmp = asset_dir / 'mode_select.bmp'
    if ms_bmp.exists():
        ms_img  = Image.open(ms_bmp)
        ms_raw  = img_to_4bpp(ms_img, 200, 112, resample=Image.NEAREST)
        ms_data = pad_to_sector(bytearray(ms_raw))
        ms_secs = len(ms_data) // 512
        if dry_run:
            print(f"  [DRY-RUN] mode_select 200×112: {len(ms_raw)} B: LBA50-{50+ms_secs-1}")
        else:
            with open(dev, 'r+b') as f:
                f.seek(50 * 512)
                f.write(ms_data)
            print(f"  OK: mode_select 200×112: {len(ms_raw)} B: LBA50-{50+ms_secs-1}")
        written.append((50, f"mode_select 200×112 ({ms_secs} sectores)"))
    else:
        print("  AVISO: mode_select.bmp no encontrado, se omite.")

    # ── Pause menu: 200×112 px generado con FreeSansBold ─────────────────────
    # LBA 72-93 (22 sectores). "REANUDAR" arriba, "SALIR" abajo; fondo negro.
    pm_bmp = asset_dir / 'pause_menu.bmp'
    if pm_bmp.exists():
        pm_img  = Image.open(pm_bmp)
        pm_raw  = img_to_4bpp(pm_img, 200, 80, resample=Image.NEAREST)
        pm_data = pad_to_sector(bytearray(pm_raw))
        pm_secs = len(pm_data) // 512
        if dry_run:
            print(f"  [DRY-RUN] pause_menu 200×80: {len(pm_raw)} B: LBA72-{72+pm_secs-1}")
        else:
            with open(dev, 'r+b') as f:
                f.seek(72 * 512)
                f.write(pm_data)
            print(f"  OK: pause_menu 200×80: {len(pm_raw)} B: LBA72-{72+pm_secs-1}")
        written.append((72, f"pause_menu 200×80 ({pm_secs} sectores)"))
    else:
        print("  AVISO: pause_menu.bmp no encontrado — generar con: python3 scripts/gen_pause_sprite.py")

    print(f"\n{'DRY-RUN completado' if dry_run else 'Escritura completada'}:")
    for lba, label in written:
        print(f"  LBA{lba}: {label}")

if __name__ == '__main__':
    main()
