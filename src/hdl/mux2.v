/**
 * @title Multiplexor 2:1 paramétrico
 * @file mux2.v
 * @brief Multiplexor genérico de 2 entradas y WIDTH bits. Combinacional, latencia cero.
 *
 * @author JustinAlfaro
 * @date 2026-04-22
 */

`timescale 1ns / 1ps

module mux2 #(
    parameter integer WIDTH = 1  ///< Ancho del bus de datos en bits
)(
    input  wire [WIDTH-1:0] d0,  ///< Entrada seleccionada cuando sel=0
    input  wire [WIDTH-1:0] d1,  ///< Entrada seleccionada cuando sel=1
    input  wire             sel, ///< Señal de selección
    output wire [WIDTH-1:0] y    ///< Salida
);
    assign y = sel ? d1 : d0;
endmodule
