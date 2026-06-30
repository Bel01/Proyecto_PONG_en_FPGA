/**
 * @title Controlador VGA 640x480 @ 60 Hz
 * @file vga_controller.v
 * @brief Genera señales de sincronismo VGA y coordenadas de píxel para resolución 640x480 a 60 Hz.
 * @details
 *   Opera en el dominio de 100 MHz usando tick_25mhz como clock enable (1 de cada 4 ciclos).
 *   HSYNC y VSYNC son activos en bajo según el estándar VGA.
 *
 *   Temporización horizontal (a 25 MHz):
 *     Visible: 640 | Porch frontal: 16 | Sync: 96 | Porch trasero: 48 | Total: 800
 *
 *   Temporización vertical (líneas):
 *     Visible: 480 | Porch frontal: 10 | Sync: 2  | Porch trasero: 33 | Total: 525
 *
 * @author JustinAlfaro
 * @date 2026-04-21
 */

`timescale 1ns / 1ps

module vga_controller (
    input  wire        clk,        ///< Reloj del sistema (100 MHz)
    input  wire        rst,        ///< Reset síncrono activo alto
    input  wire        tick_25mhz, ///< Clock enable de píxel (1 pulso cada 4 ciclos = 25 MHz efectivo)
    output reg         hsync,      ///< Sincronismo horizontal activo bajo
    output reg         vsync,      ///< Sincronismo vertical activo bajo
    output wire        blank,      ///< Alto cuando el píxel está fuera del área visible
    output reg  [9:0]  h_count,    ///< Coordenada horizontal actual [0-799]; visible: 0-639
    output reg  [9:0]  v_count     ///< Coordenada vertical actual [0-524]; visible: 0-479
);

    localparam H_VISIBLE     = 10'd640;
    localparam H_FRONT_PORCH = 10'd16;
    localparam H_SYNC_WIDTH  = 10'd96;
    localparam H_BACK_PORCH  = 10'd48;
    localparam H_TOTAL       = 10'd800;

    localparam V_VISIBLE     = 10'd480;
    localparam V_FRONT_PORCH = 10'd10;
    localparam V_SYNC_WIDTH  = 10'd2;
    localparam V_BACK_PORCH  = 10'd33;
    localparam V_TOTAL       = 10'd525;

    localparam H_SYNC_START = H_VISIBLE + H_FRONT_PORCH;
    localparam H_SYNC_END   = H_VISIBLE + H_FRONT_PORCH + H_SYNC_WIDTH;
    localparam V_SYNC_START = V_VISIBLE + V_FRONT_PORCH;
    localparam V_SYNC_END   = V_VISIBLE + V_FRONT_PORCH + V_SYNC_WIDTH;

    always @(posedge clk) begin
        if (rst) begin
            h_count <= 10'd0;
        end else if (tick_25mhz) begin
            if (h_count == H_TOTAL - 1)
                h_count <= 10'd0;
            else
                h_count <= h_count + 10'd1;
        end
    end

    always @(posedge clk) begin
        if (rst) begin
            v_count <= 10'd0;
        end else if (tick_25mhz && (h_count == H_TOTAL - 1)) begin
            if (v_count == V_TOTAL - 1)
                v_count <= 10'd0;
            else
                v_count <= v_count + 10'd1;
        end
    end

    always @(posedge clk) begin
        if (rst)
            hsync <= 1'b1;
        else if (tick_25mhz)
            hsync <= ~((h_count >= H_SYNC_START) && (h_count < H_SYNC_END));
    end

    always @(posedge clk) begin
        if (rst)
            vsync <= 1'b1;
        else if (tick_25mhz)
            vsync <= ~((v_count >= V_SYNC_START) && (v_count < V_SYNC_END));
    end

    assign blank = (h_count >= H_VISIBLE) || (v_count >= V_VISIBLE);

endmodule
