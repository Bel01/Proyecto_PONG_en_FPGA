/**
 * @title Multiplexor de salida VGA
 * @file pixel_mux.v
 * @brief Fuerza RGB a cero durante blanking; de lo contrario enruta el píxel de 12 bits a los canales VGA.
 * @details
 *   Utiliza tres instancias de mux2 (una por canal R, G, B) para mantener
 *   al multiplexor genérico como única fuente de lógica de selección.
 *
 * @author JustinAlfaro
 * @date 2026-04-22
 */

`timescale 1ns / 1ps

module pixel_mux (
    input  wire        blank,       ///< Señal de blanking activo alto (fuera del área visible)
    input  wire [11:0] pixel_data,  ///< Píxel de 12 bits desde BRAM: {R[3:0], G[3:0], B[3:0]}
    output wire [3:0]  vga_r,       ///< Canal rojo VGA [3:0]
    output wire [3:0]  vga_g,       ///< Canal verde VGA [3:0]
    output wire [3:0]  vga_b        ///< Canal azul VGA [3:0]
);
    mux2 #(.WIDTH(4)) u_r (.d0(4'h0), .d1(pixel_data[11:8]), .sel(~blank), .y(vga_r));
    mux2 #(.WIDTH(4)) u_g (.d0(4'h0), .d1(pixel_data[7:4]),  .sel(~blank), .y(vga_g));
    mux2 #(.WIDTH(4)) u_b (.d0(4'h0), .d1(pixel_data[3:0]),  .sel(~blank), .y(vga_b));
endmodule
