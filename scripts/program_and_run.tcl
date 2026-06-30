# program_and_run.tcl — Programa bitstream + ELF en una sola Nexys A7-100T (modo 1P)
#
# Uso (desde la raíz del repo):
#   xsdb scripts/program_and_run.tcl
#
# El ELF se busca en orden (Debug/ es la salida de Vitis 2024.1):
#   1. <repo_root>/../pong_workspace/pong_app/Debug/pong_app.elf  (workspace estándar)
#   2. <repo_root>/pong_workspace/pong_app/Debug/pong_app.elf     (alternativo)
#   3. <repo_root>/../pong_workspace/pong_app/build/pong_app.elf  (versiones anteriores)
#   4. <repo_root>/pong_workspace/pong_app/build/pong_app.elf     (alternativo)

set repo_root [file normalize [file join [file dirname [info script]] ".."]]
set bit       [file normalize [file join $repo_root "bin" "build_latest" "top_pong_project.bit"]]

if {![file exists $bit]} {
    puts "ERROR: Bitstream no encontrado en $bit"
    puts "       Genera primero el bitstream con:  bash scripts/build_all.sh"
    exit 1
}

set elf_candidates [list \
    [file normalize [file join $repo_root ".." "pong_workspace" "pong_app" "Debug" "pong_app.elf"]] \
    [file normalize [file join $repo_root "pong_workspace" "pong_app" "Debug" "pong_app.elf"]] \
    [file normalize [file join $repo_root ".." "pong_workspace" "pong_app" "build" "pong_app.elf"]] \
    [file normalize [file join $repo_root "pong_workspace" "pong_app" "build" "pong_app.elf"]] \
]
set elf ""
foreach c $elf_candidates {
    if {[file exists $c]} { set elf $c; break }
}

if {$elf eq ""} {
    puts "ERROR: ELF no encontrado. Compila primero con:  bash scripts/build_vitis.sh"
    exit 1
}

puts "INFO: Bitstream : $bit"
puts "INFO: ELF       : $elf"

connect
puts "INFO: Programando bitstream..."
fpga -f $bit
puts "INFO: Esperando calibración MIG DDR2 (4 s)..."
after 4000

targets -set -filter {name =~ "*Hart*"}
rst -processor
after 500
dow $elf
after 500
con

puts "DONE: Pong corriendo. SW0=OFF para modo 1P."
