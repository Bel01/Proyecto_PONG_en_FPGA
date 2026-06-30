/**
 * @title Divisor de frecuencia
 * @file div_frec.v
 * @brief Genera un pulso de un ciclo de ancho cada DIVISOR ciclos de reloj.
 * @details
 *   Usado para derivar tick_25mhz (DIVISOR=4) y tick_1hz (DIVISOR=100_000_000)
 *   desde el reloj de 100 MHz. La salida es un enable de un ciclo, no un reloj real.
 *
 * @author JustinAlfaro
 * @date 2026-04-21
 */

`timescale 1ns / 1ps

module div_freq #(
    parameter integer DIVISOR = 100_000_000  ///< Cociente de división (default: 100 MHz: 1 Hz)
)(
    input  wire clk_in,   ///< Reloj de entrada (100 MHz)
    input  wire rst,      ///< Reset síncrono activo alto
    output reg  clk_out   ///< Pulso de salida: 1 ciclo de ancho a clk_in/DIVISOR
);

    localparam integer COUNT_MAX = DIVISOR - 1;

    integer count;

    always @(posedge clk_in) begin
        if (rst) begin
            count   <= 0;
            clk_out <= 1'b0;
        end else begin
            if (count == COUNT_MAX) begin
                count   <= 0;
                clk_out <= 1'b1;
            end else begin
                count   <= count + 1;
                clk_out <= 1'b0;
            end
        end
    end

endmodule
