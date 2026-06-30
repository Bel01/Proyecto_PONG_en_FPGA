/**
 * @title Sincronizador de dos etapas
 * @file sync_signal.v
 * @brief Sincronizador de bus de WIDTH bits para prevenir metaestabilidad en entradas asíncronas.
 * @details
 *   Implementa un doble flip-flop (two-stage synchronizer) marcado con
 *   el atributo ASYNC_REG para que Vivado los coloque adyacentes y minimice MTBF.
 *   Usado principalmente para sincronizar SW[15:0] al dominio de 100 MHz.
 *
 * @author JustinAlfaro
 * @date 2026-04-21
 */

`timescale 1ns / 1ps

module sync_signal #(
    parameter integer WIDTH = 16  ///< Número de bits a sincronizar (default: 16 para SW[15:0])
)(
    input  wire             clk,      ///< Reloj del dominio destino
    input  wire             rst,      ///< Reset síncrono activo alto
    input  wire [WIDTH-1:0] async_in, ///< Bus de entrada asíncrona
    output wire [WIDTH-1:0] sync_out  ///< Bus sincronizado al dominio de clk
);

    (* ASYNC_REG = "TRUE" *) reg [WIDTH-1:0] stage1;
    (* ASYNC_REG = "TRUE" *) reg [WIDTH-1:0] stage2;

    always @(posedge clk) begin
        if (rst) begin
            stage1 <= {WIDTH{1'b0}};
            stage2 <= {WIDTH{1'b0}};
        end else begin
            stage1 <= async_in;
            stage2 <= stage1;
        end
    end

    assign sync_out = stage2;

endmodule
