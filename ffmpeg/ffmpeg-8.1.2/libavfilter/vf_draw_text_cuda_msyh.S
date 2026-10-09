/*
 * Built-in Microsoft YaHei (msyh.ttc). Assembled with .incbin so the
 * 19MB face is not expanded into a C array.
 */
        .section .rodata
        .align 4
        .global ff_vf_draw_text_cuda_msyh_data
        .global ff_vf_draw_text_cuda_msyh_end
ff_vf_draw_text_cuda_msyh_data:
        .incbin "libavfilter/draw_text_msyh.ttc"
ff_vf_draw_text_cuda_msyh_end:
