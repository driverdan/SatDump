/*
 * satdump_jpeg_prefix.h
 *
 * SatDump-local file, not part of libjpeg-turbo.
 *
 * libjpeg-turbo is compiled into libsatdump_core, which exports its symbols.
 * The process can also load a system libjpeg (through GTK's image loaders,
 * libtiff, ...) whose functions have the same names but a different ABI, and
 * the dynamic linker would then mix the two. To avoid this, every global
 * symbol of the library gets a satdump_ prefix. jconfig.h and jconfigint.h
 * include this file, so it applies to the library sources and to everything
 * that includes jpeglib.h.
 *
 * The list holds every global symbol defined by the library objects. After
 * updating the library, rebuild it and check that no unprefixed symbol is
 * left, e.g. with
 *   nm -g --defined-only <library objects> | grep -v ' satdump_'
 */

#ifndef SATDUMP_JPEG_PREFIX_H
#define SATDUMP_JPEG_PREFIX_H

#define j12copy_sample_rows                  satdump_j12copy_sample_rows
#define j12init_1pass_quantizer              satdump_j12init_1pass_quantizer
#define j12init_2pass_quantizer              satdump_j12init_2pass_quantizer
#define j12init_c_coef_controller            satdump_j12init_c_coef_controller
#define j12init_c_diff_controller            satdump_j12init_c_diff_controller
#define j12init_c_main_controller            satdump_j12init_c_main_controller
#define j12init_color_converter              satdump_j12init_color_converter
#define j12init_color_deconverter            satdump_j12init_color_deconverter
#define j12init_c_prep_controller            satdump_j12init_c_prep_controller
#define j12init_d_coef_controller            satdump_j12init_d_coef_controller
#define j12init_d_diff_controller            satdump_j12init_d_diff_controller
#define j12init_d_main_controller            satdump_j12init_d_main_controller
#define j12init_downsampler                  satdump_j12init_downsampler
#define j12init_d_post_controller            satdump_j12init_d_post_controller
#define j12init_forward_dct                  satdump_j12init_forward_dct
#define j12init_inverse_dct                  satdump_j12init_inverse_dct
#define j12init_lossless_compressor          satdump_j12init_lossless_compressor
#define j12init_lossless_decompressor        satdump_j12init_lossless_decompressor
#define j12init_merged_upsampler             satdump_j12init_merged_upsampler
#define j12init_upsampler                    satdump_j12init_upsampler
#define j16copy_sample_rows                  satdump_j16copy_sample_rows
#define j16init_c_diff_controller            satdump_j16init_c_diff_controller
#define j16init_c_main_controller            satdump_j16init_c_main_controller
#define j16init_color_converter              satdump_j16init_color_converter
#define j16init_color_deconverter            satdump_j16init_color_deconverter
#define j16init_c_prep_controller            satdump_j16init_c_prep_controller
#define j16init_d_diff_controller            satdump_j16init_d_diff_controller
#define j16init_d_main_controller            satdump_j16init_d_main_controller
#define j16init_downsampler                  satdump_j16init_downsampler
#define j16init_d_post_controller            satdump_j16init_d_post_controller
#define j16init_lossless_compressor          satdump_j16init_lossless_compressor
#define j16init_lossless_decompressor        satdump_j16init_lossless_decompressor
#define j16init_upsampler                    satdump_j16init_upsampler
#define jcopy_block_row                      satdump_jcopy_block_row
#define jcopy_sample_rows                    satdump_jcopy_sample_rows
#define jdiv_round_up                        satdump_jdiv_round_up
#define jinit_1pass_quantizer                satdump_jinit_1pass_quantizer
#define jinit_2pass_quantizer                satdump_jinit_2pass_quantizer
#define jinit_arith_decoder                  satdump_jinit_arith_decoder
#define jinit_arith_encoder                  satdump_jinit_arith_encoder
#define jinit_c_coef_controller              satdump_jinit_c_coef_controller
#define jinit_c_diff_controller              satdump_jinit_c_diff_controller
#define jinit_c_main_controller              satdump_jinit_c_main_controller
#define jinit_c_master_control               satdump_jinit_c_master_control
#define jinit_color_converter                satdump_jinit_color_converter
#define jinit_color_deconverter              satdump_jinit_color_deconverter
#define jinit_compress_master                satdump_jinit_compress_master
#define jinit_c_prep_controller              satdump_jinit_c_prep_controller
#define jinit_d_coef_controller              satdump_jinit_d_coef_controller
#define jinit_d_diff_controller              satdump_jinit_d_diff_controller
#define jinit_d_main_controller              satdump_jinit_d_main_controller
#define jinit_downsampler                    satdump_jinit_downsampler
#define jinit_d_post_controller              satdump_jinit_d_post_controller
#define jinit_forward_dct                    satdump_jinit_forward_dct
#define jinit_huff_decoder                   satdump_jinit_huff_decoder
#define jinit_huff_encoder                   satdump_jinit_huff_encoder
#define jinit_input_controller               satdump_jinit_input_controller
#define jinit_inverse_dct                    satdump_jinit_inverse_dct
#define jinit_lhuff_decoder                  satdump_jinit_lhuff_decoder
#define jinit_lhuff_encoder                  satdump_jinit_lhuff_encoder
#define jinit_lossless_compressor            satdump_jinit_lossless_compressor
#define jinit_lossless_decompressor          satdump_jinit_lossless_decompressor
#define jinit_marker_reader                  satdump_jinit_marker_reader
#define jinit_marker_writer                  satdump_jinit_marker_writer
#define jinit_master_decompress              satdump_jinit_master_decompress
#define jinit_memory_mgr                     satdump_jinit_memory_mgr
#define jinit_merged_upsampler               satdump_jinit_merged_upsampler
#define jinit_phuff_decoder                  satdump_jinit_phuff_decoder
#define jinit_phuff_encoder                  satdump_jinit_phuff_encoder
#define jinit_upsampler                      satdump_jinit_upsampler
#define jpeg12_crop_scanline                 satdump_jpeg12_crop_scanline
#define jpeg12_fdct_ifast                    satdump_jpeg12_fdct_ifast
#define jpeg12_fdct_islow                    satdump_jpeg12_fdct_islow
#define jpeg12_idct_10x10                    satdump_jpeg12_idct_10x10
#define jpeg12_idct_11x11                    satdump_jpeg12_idct_11x11
#define jpeg12_idct_12x12                    satdump_jpeg12_idct_12x12
#define jpeg12_idct_13x13                    satdump_jpeg12_idct_13x13
#define jpeg12_idct_14x14                    satdump_jpeg12_idct_14x14
#define jpeg12_idct_15x15                    satdump_jpeg12_idct_15x15
#define jpeg12_idct_16x16                    satdump_jpeg12_idct_16x16
#define jpeg12_idct_1x1                      satdump_jpeg12_idct_1x1
#define jpeg12_idct_2x2                      satdump_jpeg12_idct_2x2
#define jpeg12_idct_3x3                      satdump_jpeg12_idct_3x3
#define jpeg12_idct_4x4                      satdump_jpeg12_idct_4x4
#define jpeg12_idct_5x5                      satdump_jpeg12_idct_5x5
#define jpeg12_idct_6x6                      satdump_jpeg12_idct_6x6
#define jpeg12_idct_7x7                      satdump_jpeg12_idct_7x7
#define jpeg12_idct_9x9                      satdump_jpeg12_idct_9x9
#define jpeg12_idct_float                    satdump_jpeg12_idct_float
#define jpeg12_idct_ifast                    satdump_jpeg12_idct_ifast
#define jpeg12_idct_islow                    satdump_jpeg12_idct_islow
#define jpeg12_read_raw_data                 satdump_jpeg12_read_raw_data
#define jpeg12_read_scanlines                satdump_jpeg12_read_scanlines
#define jpeg12_skip_scanlines                satdump_jpeg12_skip_scanlines
#define jpeg12_write_raw_data                satdump_jpeg12_write_raw_data
#define jpeg12_write_scanlines               satdump_jpeg12_write_scanlines
#define jpeg16_read_scanlines                satdump_jpeg16_read_scanlines
#define jpeg16_write_scanlines               satdump_jpeg16_write_scanlines
#define jpeg_abort                           satdump_jpeg_abort
#define jpeg_abort_compress                  satdump_jpeg_abort_compress
#define jpeg_abort_decompress                satdump_jpeg_abort_decompress
#define jpeg_add_quant_table                 satdump_jpeg_add_quant_table
#define jpeg_alloc_huff_table                satdump_jpeg_alloc_huff_table
#define jpeg_alloc_quant_table               satdump_jpeg_alloc_quant_table
#define jpeg_aritab                          satdump_jpeg_aritab
#define jpeg_calc_output_dimensions          satdump_jpeg_calc_output_dimensions
#define jpeg_consume_input                   satdump_jpeg_consume_input
#define jpeg_copy_critical_parameters        satdump_jpeg_copy_critical_parameters
#define jpeg_CreateCompress                  satdump_jpeg_CreateCompress
#define jpeg_CreateDecompress                satdump_jpeg_CreateDecompress
#define jpeg_crop_scanline                   satdump_jpeg_crop_scanline
#define jpeg_default_colorspace              satdump_jpeg_default_colorspace
#define jpeg_destroy                         satdump_jpeg_destroy
#define jpeg_destroy_compress                satdump_jpeg_destroy_compress
#define jpeg_destroy_decompress              satdump_jpeg_destroy_decompress
#define jpeg_enable_lossless                 satdump_jpeg_enable_lossless
#define jpeg_fdct_float                      satdump_jpeg_fdct_float
#define jpeg_fdct_ifast                      satdump_jpeg_fdct_ifast
#define jpeg_fdct_islow                      satdump_jpeg_fdct_islow
#define jpeg_fill_bit_buffer                 satdump_jpeg_fill_bit_buffer
#define jpeg_finish_compress                 satdump_jpeg_finish_compress
#define jpeg_finish_decompress               satdump_jpeg_finish_decompress
#define jpeg_finish_output                   satdump_jpeg_finish_output
#define jpeg_free_large                      satdump_jpeg_free_large
#define jpeg_free_small                      satdump_jpeg_free_small
#define jpeg_gen_optimal_table               satdump_jpeg_gen_optimal_table
#define jpeg_get_large                       satdump_jpeg_get_large
#define jpeg_get_small                       satdump_jpeg_get_small
#define jpeg_has_multiple_scans              satdump_jpeg_has_multiple_scans
#define jpeg_huff_decode                     satdump_jpeg_huff_decode
#define jpeg_idct_10x10                      satdump_jpeg_idct_10x10
#define jpeg_idct_11x11                      satdump_jpeg_idct_11x11
#define jpeg_idct_12x12                      satdump_jpeg_idct_12x12
#define jpeg_idct_13x13                      satdump_jpeg_idct_13x13
#define jpeg_idct_14x14                      satdump_jpeg_idct_14x14
#define jpeg_idct_15x15                      satdump_jpeg_idct_15x15
#define jpeg_idct_16x16                      satdump_jpeg_idct_16x16
#define jpeg_idct_1x1                        satdump_jpeg_idct_1x1
#define jpeg_idct_2x2                        satdump_jpeg_idct_2x2
#define jpeg_idct_3x3                        satdump_jpeg_idct_3x3
#define jpeg_idct_4x4                        satdump_jpeg_idct_4x4
#define jpeg_idct_5x5                        satdump_jpeg_idct_5x5
#define jpeg_idct_6x6                        satdump_jpeg_idct_6x6
#define jpeg_idct_7x7                        satdump_jpeg_idct_7x7
#define jpeg_idct_9x9                        satdump_jpeg_idct_9x9
#define jpeg_idct_float                      satdump_jpeg_idct_float
#define jpeg_idct_ifast                      satdump_jpeg_idct_ifast
#define jpeg_idct_islow                      satdump_jpeg_idct_islow
#define jpeg_input_complete                  satdump_jpeg_input_complete
#define jpeg_make_c_derived_tbl              satdump_jpeg_make_c_derived_tbl
#define jpeg_make_d_derived_tbl              satdump_jpeg_make_d_derived_tbl
#define jpeg_mem_available                   satdump_jpeg_mem_available
#define jpeg_mem_dest                        satdump_jpeg_mem_dest
#define jpeg_mem_init                        satdump_jpeg_mem_init
#define jpeg_mem_src                         satdump_jpeg_mem_src
#define jpeg_mem_term                        satdump_jpeg_mem_term
#define jpeg_natural_order                   satdump_jpeg_natural_order
#define jpeg_nbits_table                     satdump_jpeg_nbits_table
#define jpeg_new_colormap                    satdump_jpeg_new_colormap
#define jpeg_open_backing_store              satdump_jpeg_open_backing_store
#define jpeg_quality_scaling                 satdump_jpeg_quality_scaling
#define jpeg_read_coefficients               satdump_jpeg_read_coefficients
#define jpeg_read_header                     satdump_jpeg_read_header
#define jpeg_read_icc_profile                satdump_jpeg_read_icc_profile
#define jpeg_read_raw_data                   satdump_jpeg_read_raw_data
#define jpeg_read_scanlines                  satdump_jpeg_read_scanlines
#define jpeg_resync_to_restart               satdump_jpeg_resync_to_restart
#define jpeg_save_markers                    satdump_jpeg_save_markers
#define jpeg_set_colorspace                  satdump_jpeg_set_colorspace
#define jpeg_set_defaults                    satdump_jpeg_set_defaults
#define jpeg_set_linear_quality              satdump_jpeg_set_linear_quality
#define jpeg_set_marker_processor            satdump_jpeg_set_marker_processor
#define jpeg_set_quality                     satdump_jpeg_set_quality
#define jpeg_simple_progression              satdump_jpeg_simple_progression
#define jpeg_skip_scanlines                  satdump_jpeg_skip_scanlines
#define jpeg_start_compress                  satdump_jpeg_start_compress
#define jpeg_start_decompress                satdump_jpeg_start_decompress
#define jpeg_start_output                    satdump_jpeg_start_output
#define jpeg_std_error                       satdump_jpeg_std_error
#define jpeg_stdio_dest                      satdump_jpeg_stdio_dest
#define jpeg_stdio_src                       satdump_jpeg_stdio_src
#define jpeg_suppress_tables                 satdump_jpeg_suppress_tables
#define jpeg_write_coefficients              satdump_jpeg_write_coefficients
#define jpeg_write_icc_profile               satdump_jpeg_write_icc_profile
#define jpeg_write_marker                    satdump_jpeg_write_marker
#define jpeg_write_m_byte                    satdump_jpeg_write_m_byte
#define jpeg_write_m_header                  satdump_jpeg_write_m_header
#define jpeg_write_raw_data                  satdump_jpeg_write_raw_data
#define jpeg_write_scanlines                 satdump_jpeg_write_scanlines
#define jpeg_write_tables                    satdump_jpeg_write_tables
#define jround_up                            satdump_jround_up
#define jzero_far                            satdump_jzero_far

#endif /* SATDUMP_JPEG_PREFIX_H */
