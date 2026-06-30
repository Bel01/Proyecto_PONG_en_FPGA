# program_both.tcl — Programa bitstream + ELF en las dos Nexys A7-100T (modo 2P)
#
# Uso (desde la raíz del repo):
#   xsdb scripts/program_both.tcl
#
# Seriales JTAG de las dos placas del equipo:
#   A = 210292BB3414A  (SW0=OFF: Maestro, campo izquierdo)
#   B = 210292BB376FA  (SW0=ON : Esclavo,  campo derecho)
#
# El ELF se busca en orden (Debug/ es la salida de Vitis 2024.1):
#   1. <repo_root>/../pong_workspace/pong_app/Debug/pong_app.elf  (workspace estándar)
#   2. <repo_root>/pong_workspace/pong_app/Debug/pong_app.elf     (alternativo)
#   3. <repo_root>/../pong_workspace/pong_app/build/pong_app.elf  (versiones anteriores)
#   4. <repo_root>/pong_workspace/pong_app/build/pong_app.elf     (alternativo)

set repo_root [file normalize [file join [file dirname [info script]] ".."]]
set bit       [file normalize [file join $repo_root "bin" "build_latest" "top_pong_project.bit"]]

# Búsqueda del ELF en rutas relativas al repo
set elf_candidates [list \
    [file normalize [file join $repo_root ".." "pong_workspace" "pong_app" "Debug" "pong_app.elf"]] \
    [file normalize [file join $repo_root ".." "pong_workspace" "pong_app" "build" "pong_app.elf"]] \
    [file normalize [file join $repo_root "pong_workspace" "pong_app" "Debug" "pong_app.elf"]] \
    [file normalize [file join $repo_root "pong_workspace" "pong_app" "build" "pong_app.elf"]] \
]
set elf ""
foreach c $elf_candidates {
    if {[file exists $c]} { set elf $c; break }
}

if {![file exists $bit]} {
    puts "ERROR: Bitstream no encontrado en $bit"
    puts "       Genera primero el bitstream con:  bash scripts/build_all.sh"
    exit 1
}
if {$elf eq ""} {
    puts "ERROR: ELF no encontrado. Compila primero con:  bash scripts/build_vitis.sh"
    exit 1
}

puts "INFO: Bitstream : $bit"
puts "INFO: ELF       : $elf"

set SER_A "210292BB3414A"
set SER_B "210292BB376FA"

connect
after 1000

foreach {label serial} [list A $SER_A B $SER_B] {
    puts "INFO: \[$label\] Programando bitstream..."
    targets -set -filter "name == \"xc7a100t\" && jtag_cable_serial == \"$serial\""
    fpga -f $bit
    puts "INFO: \[$label\] Bitstream OK."
}

puts "INFO: Esperando calibración MIG DDR2 (4 s)..."
after 4000

set n 0
foreach line [split [targets] "\n"] {
    if {[regexp {^\s+(\d+)\s+Hart} $line _ id]} {
        if {[catch {
            targets $id
            puts "INFO: ELF: Hart target $id"
            rst -processor
            after 3000
            # Mini-stub en LMB BRAM (0x0): lui t0,0x80000 + jalr zero,t0,0
            dow $elf
            after 300
            rwr pc 0x80000000
            con
            incr n
        } err]} { puts "WARN: target $id: $err" }
    }
}

puts "DONE: $n Hart(s) cargados."
puts "SW0=OFF: Maestro (campo izquierdo)  |  SW0=ON: Esclavo (campo derecho)"
