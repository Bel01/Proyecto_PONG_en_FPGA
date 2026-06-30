# build_bitstream.tcl
# Abre el proyecto, valida el BD, re-sintetiza, implementa y escribe bitstream.
# Genera además un CSV de utilización de recursos para evaluación.
# Uso: vivado -mode batch -source scripts/build_bitstream.tcl

set repo_path [file normalize "[file dirname [info script]]/.."]
set proj_file [file normalize "$repo_path/Projects/pong_project/pong_project.xpr"]
set out_dir   [file normalize "$repo_path/bin/build_latest"]

puts "INFO: Abriendo proyecto $proj_file ..."
open_project $proj_file

set bd_file [get_files */microblaze_v.bd]
set bd_path [file normalize $bd_file]

# Paso 1: Actualizar IPs.
puts "INFO: Actualizando IPs ..."
upgrade_ip -quiet [get_ips -all]

# Paso 2: Limpiar solo el wrapper HDL cacheado.
# La ruta real del gen de Vivado es 4 niveles arriba desde BD/:
#   BD/: repo/: Proyecto_2/: Documentos/: /home/alpha/
#: /home/alpha/pong_project.gen/
# Borramos solo hdl/ (wrapper) para forzar su regeneración.
# Los subdirectorios ip/ (DCPs de IPs OOC) se conservan para reutilización.
set bd_dir      [file dirname $bd_path]
set gen_bd_root [file normalize "$bd_dir/../../../../pong_project.gen/sources_1/bd/microblaze_v"]
set gen_hdl_dir [file normalize "$gen_bd_root/hdl"]
if {[file exists $gen_hdl_dir]} {
    file delete -force $gen_hdl_dir
    puts "INFO: Wrapper HDL cacheado eliminado: $gen_hdl_dir"
} else {
    puts "INFO: Wrapper HDL no existe, nada que eliminar."
}

# Paso 3: Crear XCI de IPs faltantes (not_calib_0, reset_or_0).
# Vivado agrega estas cuando se abre el BD en GUI y se valida. Son instancias
# de util_vector_logic (NOT y OR) con C_SIZE=1. El BD las referencia pero sus
# XCI no están versionados. Si no existen, se crean aquí para que
# validate_bd_design y generate_target puedan usarlos.
set bd_ip_dir [file dirname $bd_path]/ip
foreach {ip_name op} {microblaze_v_not_calib_0 not microblaze_v_reset_or_0 or} {
    set xci_path [file normalize "$bd_ip_dir/$ip_name/$ip_name.xci"]
    if {![file exists $xci_path]} {
        puts "INFO: Creando IP faltante: $ip_name (util_vector_logic C_OPERATION=$op) ..."
        create_ip -name util_vector_logic -vendor xilinx.com -library ip \
            -version 2.0 -module_name $ip_name -dir $bd_ip_dir -quiet
        set_property -dict [list CONFIG.C_OPERATION $op CONFIG.C_SIZE {1}] \
            [get_ips $ip_name]
        generate_target all [get_ips $ip_name] -quiet
        puts "INFO: $ip_name creado en $bd_ip_dir/$ip_name/"
    } else {
        puts "INFO: $ip_name ya existe, omitiendo creación."
    }
}

# Paso 4: Abrir BD, validar, generar productos HDL y registrar wrapper.
# validate_bd_design propaga parámetros AXI y conserva auto_pc en couplers.
# generate_target se llama con el BD abierto para que Vivado registre
# correctamente el wrapper en el proyecto.
# Se fuerza re-registro del wrapper + update_compile_order para que
# synth_1 encuentre microblaze_v_wrapper al elaborar top_pong_project.v.
puts "INFO: Abriendo y validando Block Design ..."
open_bd_design $bd_file
validate_bd_design
save_bd_design
puts "INFO: Generando productos del Block Design (BD abierto) ..."
generate_target all [get_files */microblaze_v.bd] -force
close_bd_design [current_bd_design]

# Forzar re-registro del wrapper y actualizar compile order
set wrapper_files [get_files -quiet *microblaze_v_wrapper*]
if {[llength $wrapper_files] > 0} {
    remove_files $wrapper_files
}
set wrapper_path [make_wrapper -files $bd_file -top -force]
add_files -norecurse $wrapper_path
set_property top top_pong_project [current_fileset]
update_compile_order -fileset sources_1
puts "INFO: Wrapper registrado: $wrapper_path"

# Paso 6: Resetear solo synth_1 e impl_1 (NO los runs OOC).
# Los runs OOC (auto_pc, auto_cc, xbar_0, etc.) solo se re-ejecutan si su
# XCI cambió (Vivado lo detecta automáticamente). Resetearlos fuerza
# re-síntesis completa de todos los IPs, lo que tarda 3-4 horas innecesariamente.
puts "INFO: Reseteando synth_1 e impl_1 ..."
reset_run synth_1
reset_run impl_1

puts "INFO: Lanzando síntesis (synth_1) ..."
launch_runs synth_1 -jobs 2
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} {
    puts "ERROR: Síntesis falló."
    exit 1
}
puts "INFO: Síntesis completada."

# Esperar todos los runs OOC antes de implementación para evitar DRC INBB-3.
puts "INFO: Esperando runs OOC pendientes ..."
foreach r [get_runs -filter {IS_SYNTHESIS == 1 && NAME != "synth_1"}] {
    if {[get_property PROGRESS $r] != "100%"} {
        puts "INFO:   waiting on $r ..."
        wait_on_run $r
    }
}
puts "INFO: Todos los runs OOC completos."

puts "INFO: Lanzando implementación + write_bitstream (impl_1) ..."
launch_runs impl_1 -to_step write_bitstream -jobs 2
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} {
    puts "ERROR: Implementación falló."
    exit 1
}
puts "INFO: Implementación completada."

# Copiar bitstream a bin/build_latest
file mkdir $out_dir
set bit_src [get_property DIRECTORY [get_runs impl_1]]/top_pong_project.bit
file copy -force $bit_src $out_dir/top_pong_project.bit
puts "INFO: Bitstream copiado a $out_dir/top_pong_project.bit"

# Generar reporte de utilización de recursos para evaluación.
set rpt_out $out_dir/resource_usage.rpt
set csv_out $out_dir/resource_usage.csv
open_run impl_1
report_utilization -file $rpt_out
puts "INFO: Reporte de utilización escrito en $rpt_out"

set fh [open $rpt_out r]
set content [read $fh]
close $fh

set csv_fh [open $csv_out w]
puts $csv_fh "Recurso,Usado,Disponible,Porcentaje"
foreach line [split $content "\n"] {
    if {[regexp {^\|\s+([^\|]+?)\s+\|\s+(\d+)\s+\|[^\|]*\|[^\|]*\|\s+(\d+)\s+\|\s+([\d.]+)\s+\|} \
            $line _ name used avail pct]} {
        set name [string trim $name]
        if {$name ne ""} {
            puts $csv_fh "\"$name\",$used,$avail,$pct"
        }
    }
}
close $csv_fh
puts "INFO: CSV de recursos exportado a $csv_out"

close_project
puts "DONE: Bitstream listo."
