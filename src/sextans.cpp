#include <ap_int.h>
#include <cstdio>
#include <cstring>
#include <cassert>

#include <tapa.h>

#include "sextans.h"

const int WINDOW_SIZE = 4096;
const int DEP_DIST_LOAD_STORE = 10;
const int B_PARTITION_FACTOR = 4;
const int URAM_DEPTH = 12288;

void read_edge_list_ptr(
	const ap_uint<32> num_ite_in,
	const ap_uint<32> M_in,
	const ap_uint<32> P_N_in, // bit 31 - 16: repeat time, bit 15 - 0: N
	const ap_uint<32> K_in,
	const ap_uint<32> alpha_u_in,
	const ap_uint<32> beta_u_in,
	tapa::mmap<const ap_uint<32>> edge_list_ptr,
	tapa::ostream<ap_uint<32>> & fifo_edge_list_ptr
	) {
	const ap_uint<32> num_ite = num_ite_in;
	fifo_edge_list_ptr.write(num_ite);

	const ap_uint<32> M = M_in;
	fifo_edge_list_ptr.write(M);

	const ap_uint<32> N = P_N_in & ((ap_uint<32>) 0x0000FFFF);
	fifo_edge_list_ptr.write(P_N_in);

	const ap_uint<32> K = K_in;
	fifo_edge_list_ptr.write(K);

	fifo_edge_list_ptr.write(alpha_u_in);
	fifo_edge_list_ptr.write(beta_u_in);

	const ap_uint<16> N16 = P_N_in(31, 16);
	const ap_uint<16> rp_time = (N16 == 0)? ((ap_uint<16>) 1) : N16;

	l_rp: for(ap_uint<16> rp = 0; rp < rp_time; rp++) {
#pragma HLS loop_flatten off
#pragma HLS loop_tripcount min=1 max=16
		l_N: for(ap_uint<32> nn = 0; nn < (N + 7)/8; nn++) {
#pragma HLS loop_flatten off
#pragma HLS loop_tripcount min=1 max=16
			rd_ptr: for(ap_uint<32> i = 0; i < num_ite + 1; i++) {
#pragma HLS loop_tripcount min=1 max=800
#pragma HLS pipeline II=1
				ap_uint<32> tmp = edge_list_ptr[i];
				fifo_edge_list_ptr.write(tmp);
			}
		}
	}
}

void read_A(
	tapa::mmap<const ap_uint<512>> A,
	tapa::ostream<ap_uint<512>> & fifo_A,
	const ap_uint<32> A_len,
	const ap_uint<32> P_N
	) {
	const ap_uint<16> N16 = P_N(31, 16);
	const ap_uint<16> rp_time = (N16 == 0)? ((ap_uint<16>) 1) : N16;
	const ap_uint<32> N = P_N & ((ap_uint<32>) 0x0000FFFF);

	l_rp: for(ap_uint<16> rp = 0; rp < rp_time; rp++) {
#pragma HLS loop_flatten off
#pragma HLS loop_tripcount min=1 max=16
		l_N: for(ap_uint<32> nn = 0; nn < (N + 7)/8; nn++) {
#pragma HLS loop_flatten off
#pragma HLS loop_tripcount min=1 max=16
			rd_A: for(ap_uint<32> i = 0; i < A_len; i++) {
#pragma HLS loop_tripcount min=1 max=10000
#pragma HLS pipeline II=1
				ap_uint<512> tmp_A = A[i];
				fifo_A.write(tmp_A);
			}
		}
	}
}

void read_B(
	tapa::mmap<const ap_uint<512>> B,
	tapa::ostream<ap_uint<512>> & fifo_B,
	const ap_uint<32> K,
	const ap_uint<32> P_N
	) {
	const ap_uint<16> N16 = P_N(31, 16);
	const ap_uint<16> rp_time = (N16 == 0)? ((ap_uint<16>) 1) : N16;
	const ap_uint<32> N = P_N & ((ap_uint<32>) 0x0000FFFF);
	const ap_uint<32> num_ite_B = ((K + 7) / 8) * ((N + 7) / 8);

	l_rp: for(ap_uint<16> rp = 0; rp < rp_time; rp++) {
#pragma HLS loop_flatten off
#pragma HLS loop_tripcount min=1 max=16
		rd_B: for(ap_uint<32> i = 0; i < num_ite_B; i++) {
#pragma HLS loop_tripcount min=1 max=500000
#pragma HLS pipeline II=1
			ap_uint<512> tmp_B = B[i];
			fifo_B.write(tmp_B);
		}
	}
}

void PU2core(
    ap_uint<18> & addr_c,
    float & a_val_f,
    float & b_val_d0_f,
    float & b_val_d1_f,

    ap_uint<64> * local_C_pe0_d0_d1
    ) {
#pragma HLS inline
    ap_uint<64> c_val_d0_d1_u64 = local_C_pe0_d0_d1[addr_c];

    ap_uint<32> c_val_d0_u = c_val_d0_d1_u64(31,  0);
    ap_uint<32> c_val_d1_u = c_val_d0_d1_u64(63, 32);

    float c_val_d0_f = tapa::bit_cast<float>(c_val_d0_u);
    float c_val_d1_f = tapa::bit_cast<float>(c_val_d1_u);

    c_val_d0_f += tapa::reg(a_val_f) * b_val_d0_f;
    c_val_d1_f += tapa::reg(a_val_f) * b_val_d1_f;

    c_val_d0_u = tapa::bit_cast<ap_uint<32>>(c_val_d0_f);
    c_val_d1_u = tapa::bit_cast<ap_uint<32>>(c_val_d1_f);

    c_val_d0_d1_u64(31,  0) = c_val_d0_u;
    c_val_d0_d1_u64(63, 32) = c_val_d1_u;

    local_C_pe0_d0_d1[addr_c] = c_val_d0_d1_u64;
}

void PEcore(
    ap_uint<14> & addr_b,
    ap_uint<18> & addr_c,
    ap_uint<32> & a_val_u,
    ap_uint<64> local_C[NUM_CH_C / 2][URAM_DEPTH],
		ap_uint<32> local_B[8][WINDOW_SIZE]
    ) {
#pragma HLS inline
    if (addr_c != ((ap_uint<18>) 0x3FFFF)) {
        float a_val_f = tapa::bit_cast<float>(a_val_u);

        ap_uint<32> b_val_d0_u = local_B[0][addr_b];
        ap_uint<32> b_val_d1_u = local_B[1][addr_b];
        ap_uint<32> b_val_d2_u = local_B[2][addr_b];
        ap_uint<32> b_val_d3_u = local_B[3][addr_b];
        ap_uint<32> b_val_d4_u = local_B[4][addr_b];
        ap_uint<32> b_val_d5_u = local_B[5][addr_b];
        ap_uint<32> b_val_d6_u = local_B[6][addr_b];
        ap_uint<32> b_val_d7_u = local_B[7][addr_b];

        float b_val_d0_f = tapa::bit_cast<float>(b_val_d0_u);
        float b_val_d1_f = tapa::bit_cast<float>(b_val_d1_u);
        float b_val_d2_f = tapa::bit_cast<float>(b_val_d2_u);
        float b_val_d3_f = tapa::bit_cast<float>(b_val_d3_u);
        float b_val_d4_f = tapa::bit_cast<float>(b_val_d4_u);
        float b_val_d5_f = tapa::bit_cast<float>(b_val_d5_u);
        float b_val_d6_f = tapa::bit_cast<float>(b_val_d6_u);
        float b_val_d7_f = tapa::bit_cast<float>(b_val_d7_u);

        PU2core(
            addr_c,
            a_val_f,
            b_val_d0_f,
            b_val_d1_f,
            local_C[0]
            );

        PU2core(
            addr_c,
            a_val_f,
            b_val_d2_f,
            b_val_d3_f,
            local_C[1]
            );

        PU2core(
            addr_c,
            a_val_f,
            b_val_d4_f,
            b_val_d5_f,
            local_C[2]
            );

        PU2core(
            addr_c,
            a_val_f,
            b_val_d6_f,
            b_val_d7_f,
            local_C[3]
            );
    }
}


void peg16mult(
	ap_uint<512> opa512,
	ap_uint<32> alpha_u,
	ap_uint<512> & mult512
	) {
#pragma HLS inline
		float alpha_f = tapa::bit_cast<float>(alpha_u);
		ap_uint<512> c_out;

		float op_a[16];
#pragma HLS array_partition variable=op_a complete
		float op_result[16];
#pragma HLS array_partition variable=op_result complete

		for(ap_uint<5> p = 0; p < 16; ++p) {
			op_a[p]      = tapa::bit_cast<float>(opa512(31 + p * 32, p * 32).to_uint());
			op_result[p] = tapa::reg(alpha_f) * op_a[p];
			c_out(31 + p * 32, p * 32) = tapa::bit_cast<ap_uint<32>>(op_result[p]);
		}
		mult512 = tapa::reg(c_out);
}

void PEG(
	tapa::istream<ap_uint<32>> & fifo_inst,
	tapa::istream<ap_uint<512>> & fifo_A,
	tapa::istreams<ap_uint<512>, NUM_CH_B> & fifo_B_x, // [256(16)] * 2, 2: dim d
	tapa::ostream<ap_uint<32>> & fifo_inst_out, // to next PE
	tapa::ostreams<ap_uint<512>, NUM_CH_B> & fifo_B_out_x, // output to next PE [256(16)] * 2, 2: dim d
	tapa::ostream<ap_uint<512>> & fifo_C_out0 // [64(32bits * 2.0)] * 8 dims
	) {
#pragma HLS inline off
	ap_uint<32> NUM_ITE;
	ap_uint<32> M;
	ap_uint<32> P_N;
	ap_uint<32> K;
	ap_uint<32> alpha_u;
	ap_uint<32> beta_u;

	ap_uint<32> parameter;
	w_ITE: for (ap_uint<3> i = 0; i < 6; ) {
#pragma HLS loop_tripcount min=1 max=10
#pragma HLS pipeline II=1
		bool parameter_ready = fifo_inst.try_read(parameter);
		if (parameter_ready) {
			ap_uint<32> parameter_dealy = tapa::reg(parameter);
			switch (i) {
				case 0: NUM_ITE = parameter_dealy; break;
				case 1: M = parameter_dealy; break;
				case 2: P_N = parameter_dealy; break;
				case 3: K = parameter_dealy; break;
				case 4: alpha_u = parameter_dealy; break;
				case 5: beta_u = parameter_dealy; break;
			}
			++i;
			fifo_inst_out.write(parameter_dealy);
		}
	}

	const ap_uint<16> N16 = P_N(31, 16);
	const ap_uint<16> rp_time = (N16 == 0)? ((ap_uint<16>) 1) : N16;
	const ap_uint<32> N = P_N & ((ap_uint<32>) 0x0000FFFF);

	//define local C buffer and pragma to URAM
	ap_uint<64> local_C[NUM_CH_SPARSE][NUM_CH_C / 2][URAM_DEPTH];
#pragma HLS bind_storage variable=local_C type=RAM_2P impl=URAM
#pragma HLS array_partition complete variable=local_C dim=1
#pragma HLS array_partition complete variable=local_C dim=2

	l_rp: for(ap_uint<16> rp = 0; rp < rp_time; rp++) {
#pragma HLS loop_flatten off
#pragma HLS loop_tripcount min=1 max=16
		l_N: for(ap_uint<32> nn = 0; nn < (N + 7)/8; nn++) {
#pragma HLS loop_flatten off
#pragma HLS loop_tripcount min=1 max=16

			//init local C
			init_C: for (ap_uint<32> i = 0; i < ((M + 63) / 64); ++i) {
#pragma HLS loop_tripcount min=1 max=800
#pragma HLS pipeline II=1
				for (int j = 0; j < NUM_CH_SPARSE; ++j) {
					for (int k = 0; k < NUM_CH_C / 2; ++k) {
						local_C[j][k][i] = 0;
					}
				}
			}
			//define local B buffer and pragma local B buffer if partition factor > 1

			ap_uint<32> local_B[8/2][8][WINDOW_SIZE];
#pragma HLS bind_storage variable=local_B latency=3
#pragma HLS array_partition variable=local_B complete dim=1
#pragma HLS array_partition variable=local_B complete dim=2
#pragma HLS array_partition variable=local_B cyclic factor=B_PARTITION_FACTOR dim=3

			ap_uint<32> start_32;
			bool start_32_ready = false;
			w1: while(!start_32_ready) {
#pragma HLS loop_tripcount min=1 max=10
#pragma HLS pipeline II=1
				start_32_ready = fifo_inst.try_read(start_32);
			};

			fifo_inst_out.write(start_32);

			main: for (ap_uint<32> i = 0; i < NUM_ITE; ++i) {
#pragma HLS loop_tripcount min=1 max=49

				// fill onchip B
				ap_uint<512> b_512_x0;
				ap_uint<512> b_512_x1;
				ap_uint<512> b_512_x2;
				ap_uint<512> b_512_x3;

				bool b_512_x0_ready = false;
				bool b_512_x1_ready = false;
				bool b_512_x2_ready = false;
				bool b_512_x3_ready = false;

				read_B: for (ap_uint<14> j = 0; (j < WINDOW_SIZE/8) && (j < (K + 7) / 8 - i*WINDOW_SIZE/8); ) {
#pragma HLS loop_tripcount min=1 max=512
#pragma HLS pipeline II = 1
					if (!b_512_x0_ready) {
						b_512_x0_ready = fifo_B_x[0].try_read(b_512_x0);
					}
					if (!b_512_x1_ready) {
						b_512_x1_ready = fifo_B_x[1].try_read(b_512_x1);
					}
					if (!b_512_x2_ready) {
						b_512_x2_ready = fifo_B_x[2].try_read(b_512_x2);
					}
					if (!b_512_x3_ready) {
						b_512_x3_ready = fifo_B_x[3].try_read(b_512_x3);
					}

					bool b_2048_ready = b_512_x0_ready && b_512_x1_ready && b_512_x2_ready && b_512_x3_ready;

					if (b_2048_ready) {
						ap_uint<512> b_512_x0_delay = tapa::reg(b_512_x0);
						ap_uint<512> b_512_x1_delay = tapa::reg(b_512_x1);
						ap_uint<512> b_512_x2_delay = tapa::reg(b_512_x2);
						ap_uint<512> b_512_x3_delay = tapa::reg(b_512_x3);

						fifo_B_out_x[0].write(b_512_x0_delay);
						fifo_B_out_x[1].write(b_512_x1_delay);
						fifo_B_out_x[2].write(b_512_x2_delay);
						fifo_B_out_x[3].write(b_512_x3_delay);

						read_B_p: for (ap_uint<4> k = 0; k < 8; ++k) {
							ap_uint<32> b_pe_d0 = b_512_x0_delay(31 + k * 32 +   0,  k * 32 +   0);
							ap_uint<32> b_pe_d1 = b_512_x0_delay(31 + k * 32 + 256,  k * 32 + 256);
							ap_uint<32> b_pe_d2 = b_512_x1_delay(31 + k * 32 +   0,  k * 32 +   0);
							ap_uint<32> b_pe_d3 = b_512_x1_delay(31 + k * 32 + 256,  k * 32 + 256);
							ap_uint<32> b_pe_d4 = b_512_x2_delay(31 + k * 32 +   0,  k * 32 +   0);
							ap_uint<32> b_pe_d5 = b_512_x2_delay(31 + k * 32 + 256,  k * 32 + 256);
							ap_uint<32> b_pe_d6 = b_512_x3_delay(31 + k * 32 +   0,  k * 32 +   0);
							ap_uint<32> b_pe_d7 = b_512_x3_delay(31 + k * 32 + 256,  k * 32 + 256);

							local_B[0][0][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d0;
							local_B[0][1][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d1;
							local_B[0][2][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d2;
							local_B[0][3][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d3;
							local_B[0][4][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d4;
							local_B[0][5][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d5;
							local_B[0][6][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d6;
							local_B[0][7][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d7;

							local_B[1][0][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d0;
							local_B[1][1][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d1;
							local_B[1][2][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d2;
							local_B[1][3][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d3;
							local_B[1][4][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d4;
							local_B[1][5][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d5;
							local_B[1][6][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d6;
							local_B[1][7][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d7;

							local_B[2][0][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d0;
							local_B[2][1][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d1;
							local_B[2][2][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d2;
							local_B[2][3][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d3;
							local_B[2][4][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d4;
							local_B[2][5][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d5;
							local_B[2][6][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d6;
							local_B[2][7][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d7;

							local_B[3][0][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d0;
							local_B[3][1][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d1;
							local_B[3][2][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d2;
							local_B[3][3][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d3;
							local_B[3][4][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d4;
							local_B[3][5][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d5;
							local_B[3][6][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d6;
							local_B[3][7][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d7;
						}
						b_512_x0_ready = false;
						b_512_x1_ready = false;
						b_512_x2_ready = false;
						b_512_x3_ready = false;
						++j;
					}
				}

				// computation
				ap_uint<32> end_32;
				bool end_32_ready = false;
				w2: while(!end_32_ready) {
#pragma HLS loop_tripcount min=1 max=10
#pragma HLS pipeline II=1
					end_32_ready = fifo_inst.try_read(end_32);
				};

				fifo_inst_out.write(end_32);

				computation: for (ap_uint<32> j = start_32; j < end_32; ) {
#pragma HLS loop_tripcount min=1 max=200
#pragma HLS pipeline II=1
#pragma HLS dependence true variable=local_C distance=DEP_DIST_LOAD_STORE

					ap_uint<512> a_pes;
					bool a_pes_ready = fifo_A.try_read(a_pes);

					if (a_pes_ready) {
						ap_uint<512> a_pes_delay = tapa::reg(a_pes);

						ap_uint<64> a[8];
#pragma HLS array_partition variable=a complete

						ap_uint<14> a_col[8];
#pragma HLS array_partition variable=a_col complete

						ap_uint<18> a_row[8];
#pragma HLS array_partition variable=a_row complete

						ap_uint<32> a_val[8];
#pragma HLS array_partition variable=a_val complete

						for (ap_uint<4> p = 0; p < 8; ++p) {
							a[p] = a_pes_delay(63 + p * 64, p * 64);
						}

						for (ap_uint<4> p = 0; p < 8; ++p) {
							a_col[p] = (a[p])(63, 50);
							a_row[p] = (a[p])(49, 32);
							a_val[p] = (a[p])(31,  0);
						}

						// PE process
						for (int l = 0; l < NUM_CH_SPARSE; ++l) {
							PEcore(
								a_col[l],
								a_row[l],
								a_val[l],
								local_C[l],
								local_B[l/2]
								);
						}

						++j;
					}
				}
				start_32 = end_32;
			}

			ap_uint<512> out_u0;

			//write C fifo
			write_C_outer: for (ap_uint<32> i = 0; i < (M + 15)/16; ++i) {
#pragma HLS loop_tripcount min=1 max=1800
#pragma HLS pipeline II=1
				ap_uint<64> u_64_pe_d[2][4];
#pragma HLS array_partition variable=u_64_pe_d complete

				ap_uint<32> u_32_d_pe[8][2];
#pragma HLS array_partition variable=u_32_d_pe complete

				for (int pe = 0; pe < 2; ++pe) {
					for (int d = 0; d < NUM_CH_B; ++d) {
						u_64_pe_d[pe][d] = local_C[i % NUM_CH_B * 2 + pe][d][i / NUM_CH_B];
					}
				}

				for (ap_uint<4> pe = 0; pe < 2; ++pe) {
					for (ap_uint<4> d = 0; d < 4; ++d) {
						u_32_d_pe[2 * d    ][pe] = (u_64_pe_d[pe][d])(31,  0);
						u_32_d_pe[2 * d + 1][pe] = (u_64_pe_d[pe][d])(63, 32);
					}
				}

				for (ap_uint<4> d = 0; d < 8; ++d) {
					for (ap_uint<4> pe = 0; pe < 2; ++pe) {
						out_u0(31 + pe * 32 + d * 64, pe * 32 + d * 64) = u_32_d_pe[d + 0][pe];
					}
				}

				ap_uint<512> out_u0_mult;
				peg16mult(out_u0, alpha_u, out_u0_mult);
				fifo_C_out0.write(out_u0_mult);
			}
		}
	}
}


void PEG_last(
	tapa::istream<ap_uint<32>> & fifo_inst,
	tapa::istream<ap_uint<512>> & fifo_A,
	tapa::istreams<ap_uint<512>, NUM_CH_B> & fifo_B_x, // [256(16)] * 2, 2: dim d
	tapa::ostream<ap_uint<512>> & fifo_C_out0 // [64(32bits * 2.0)] * 8 dims
	) {
#pragma HLS inline off
	ap_uint<32> NUM_ITE;
	ap_uint<32> M;
	ap_uint<32> P_N;
	ap_uint<32> K;
	ap_uint<32> alpha_u;
	ap_uint<32> beta_u;

	ap_uint<32> parameter;
	w_ITE: for (ap_uint<3> i = 0; i < 6; ) {
#pragma HLS loop_tripcount min=1 max=10
#pragma HLS pipeline II=1
		bool parameter_ready = fifo_inst.try_read(parameter);
		if (parameter_ready) {
			ap_uint<32> parameter_dealy = tapa::reg(parameter);
			switch (i) {
				case 0: NUM_ITE = parameter_dealy; break;
				case 1: M = parameter_dealy; break;
				case 2: P_N = parameter_dealy; break;
				case 3: K = parameter_dealy; break;
				case 4: alpha_u = parameter_dealy; break;
				case 5: beta_u = parameter_dealy; break;
			}
			++i;
		}
	}

	const ap_uint<16> N16 = P_N(31, 16);
	const ap_uint<16> rp_time = (N16 == 0)? ((ap_uint<16>) 1) : N16;
	const ap_uint<32> N = P_N & ((ap_uint<32>) 0x0000FFFF);

	ap_uint<512> MN512;
	MN512(31,  0) = M;
	MN512(63, 32) = P_N;
	MN512(95, 64) = beta_u;
	fifo_C_out0.write(MN512);

	//define local C buffer and pragma to URAM
	ap_uint<64> local_C[NUM_CH_SPARSE][NUM_CH_C / 2][URAM_DEPTH];
#pragma HLS bind_storage variable=local_C type=RAM_2P impl=URAM
#pragma HLS array_partition complete variable=local_C dim=1
#pragma HLS array_partition complete variable=local_C dim=2

	l_rp: for(ap_uint<16> rp = 0; rp < rp_time; rp++) {
#pragma HLS loop_flatten off
#pragma HLS loop_tripcount min=1 max=16
		l_N: for(ap_uint<32> nn = 0; nn < (N + 7)/8; nn++) {
#pragma HLS loop_flatten off
#pragma HLS loop_tripcount min=1 max=16

			//init local C
			init_C: for (ap_uint<32> i = 0; i < ((M + 63) / 64); ++i) {
#pragma HLS loop_tripcount min=1 max=800
#pragma HLS pipeline II=1
				for (int j = 0; j < NUM_CH_SPARSE; ++j) {
					for (int k = 0; k < NUM_CH_C / 2; ++k) {
						local_C[j][k][i] = 0;
					}
				}
			}
			//define local B buffer and pragma local B buffer if partition factor > 1

			ap_uint<32> local_B[8/2][8][WINDOW_SIZE];
#pragma HLS bind_storage variable=local_B latency=3
#pragma HLS array_partition variable=local_B complete dim=1
#pragma HLS array_partition variable=local_B complete dim=2
#pragma HLS array_partition variable=local_B cyclic factor=B_PARTITION_FACTOR dim=3

			ap_uint<32> start_32;
			bool start_32_ready = false;
			w1: while(!start_32_ready) {
#pragma HLS loop_tripcount min=1 max=10
#pragma HLS pipeline II=1
				start_32_ready = fifo_inst.try_read(start_32);
			};

			main: for (ap_uint<32> i = 0; i < NUM_ITE; ++i) {
#pragma HLS loop_tripcount min=1 max=49

				// fill onchip B
				ap_uint<512> b_512_x0;
				ap_uint<512> b_512_x1;
				ap_uint<512> b_512_x2;
				ap_uint<512> b_512_x3;

				bool b_512_x0_ready = false;
				bool b_512_x1_ready = false;
				bool b_512_x2_ready = false;
				bool b_512_x3_ready = false;

				read_B: for (ap_uint<14> j = 0; (j < WINDOW_SIZE/8) && (j < (K + 7) / 8 - i*WINDOW_SIZE/8); ) {
#pragma HLS loop_tripcount min=1 max=512
#pragma HLS pipeline II=1
					if (!b_512_x0_ready) {
						b_512_x0_ready = fifo_B_x[0].try_read(b_512_x0);
					}
					if (!b_512_x1_ready) {
						b_512_x1_ready = fifo_B_x[1].try_read(b_512_x1);
					}
					if (!b_512_x2_ready) {
						b_512_x2_ready = fifo_B_x[2].try_read(b_512_x2);
					}
					if (!b_512_x3_ready) {
						b_512_x3_ready = fifo_B_x[3].try_read(b_512_x3);
					}

					bool b_2048_ready = b_512_x0_ready && b_512_x1_ready && b_512_x2_ready && b_512_x3_ready;

					if (b_2048_ready) {
						ap_uint<512> b_512_x0_delay = tapa::reg(b_512_x0);
						ap_uint<512> b_512_x1_delay = tapa::reg(b_512_x1);
						ap_uint<512> b_512_x2_delay = tapa::reg(b_512_x2);
						ap_uint<512> b_512_x3_delay = tapa::reg(b_512_x3);


						read_B_p: for (ap_uint<4> k = 0; k < 8; ++k) {
							ap_uint<32> b_pe_d0 = b_512_x0_delay(31 + k * 32 +   0,  k * 32 +   0);
							ap_uint<32> b_pe_d1 = b_512_x0_delay(31 + k * 32 + 256,  k * 32 + 256);
							ap_uint<32> b_pe_d2 = b_512_x1_delay(31 + k * 32 +   0,  k * 32 +   0);
							ap_uint<32> b_pe_d3 = b_512_x1_delay(31 + k * 32 + 256,  k * 32 + 256);
							ap_uint<32> b_pe_d4 = b_512_x2_delay(31 + k * 32 +   0,  k * 32 +   0);
							ap_uint<32> b_pe_d5 = b_512_x2_delay(31 + k * 32 + 256,  k * 32 + 256);
							ap_uint<32> b_pe_d6 = b_512_x3_delay(31 + k * 32 +   0,  k * 32 +   0);
							ap_uint<32> b_pe_d7 = b_512_x3_delay(31 + k * 32 + 256,  k * 32 + 256);

							local_B[0][0][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d0;
							local_B[0][1][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d1;
							local_B[0][2][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d2;
							local_B[0][3][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d3;
							local_B[0][4][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d4;
							local_B[0][5][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d5;
							local_B[0][6][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d6;
							local_B[0][7][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d7;

							local_B[1][0][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d0;
							local_B[1][1][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d1;
							local_B[1][2][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d2;
							local_B[1][3][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d3;
							local_B[1][4][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d4;
							local_B[1][5][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d5;
							local_B[1][6][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d6;
							local_B[1][7][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d7;

							local_B[2][0][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d0;
							local_B[2][1][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d1;
							local_B[2][2][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d2;
							local_B[2][3][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d3;
							local_B[2][4][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d4;
							local_B[2][5][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d5;
							local_B[2][6][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d6;
							local_B[2][7][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d7;

							local_B[3][0][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d0;
							local_B[3][1][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d1;
							local_B[3][2][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d2;
							local_B[3][3][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d3;
							local_B[3][4][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d4;
							local_B[3][5][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d5;
							local_B[3][6][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d6;
							local_B[3][7][tapa::reg(tapa::reg(j)) * 8 + k] = b_pe_d7;
						}
						b_512_x0_ready = false;
						b_512_x1_ready = false;
						b_512_x2_ready = false;
						b_512_x3_ready = false;
						++j;
					}
				}

				// computation
				ap_uint<32> end_32;
				bool end_32_ready = false;
				w2: while(!end_32_ready) {
#pragma HLS loop_tripcount min=1 max=10
#pragma HLS pipeline II=1
					end_32_ready = fifo_inst.try_read(end_32);
				};

				computation: for (ap_uint<32> j = start_32; j < end_32; ) {
#pragma HLS loop_tripcount min=1 max=200
#pragma HLS pipeline II=1
#pragma HLS dependence true variable=local_C distance=DEP_DIST_LOAD_STORE

					ap_uint<512> a_pes;
					bool a_pes_ready = fifo_A.try_read(a_pes);

					if (a_pes_ready) {
						ap_uint<512> a_pes_delay = tapa::reg(a_pes);

						ap_uint<64> a[8];
#pragma HLS array_partition variable=a complete

						ap_uint<14> a_col[8];
#pragma HLS array_partition variable=a_col complete

						ap_uint<18> a_row[8];
#pragma HLS array_partition variable=a_row complete

						ap_uint<32> a_val[8];
#pragma HLS array_partition variable=a_val complete

						for (ap_uint<4> p = 0; p < 8; ++p) {
							a[p] = a_pes_delay(63 + p * 64, p * 64);
						}

						for (ap_uint<4> p = 0; p < 8; ++p) {
							a_col[p] = (a[p])(63, 50);
							a_row[p] = (a[p])(49, 32);
							a_val[p] = (a[p])(31,  0);
						}

						// PE process
						for (int l = 0; l < NUM_CH_SPARSE; ++l) {
							PEcore(
								a_col[l],
								a_row[l],
								a_val[l],
								local_C[l],
								local_B[l/2]
								);
						}

						++j;
					}
				}
				start_32 = end_32;
			}

			ap_uint<512> out_u0;

			//write C fifo
			write_C_outer: for (ap_uint<32> i = 0; i < (M + 15)/16; ++i) {
#pragma HLS loop_tripcount min=1 max=1800
#pragma HLS pipeline II=1
				ap_uint<64> u_64_pe_d[2][4];
#pragma HLS array_partition variable=u_64_pe_d complete

				ap_uint<32> u_32_d_pe[8][2];
#pragma HLS array_partition variable=u_32_d_pe complete

				for (int pe = 0; pe < 2; ++pe) {
					for (int d = 0; d < NUM_CH_B; ++d) {
						u_64_pe_d[pe][d] = local_C[i % NUM_CH_B * 2 + pe][d][i / NUM_CH_B];
					}
				}

				for (ap_uint<4> pe = 0; pe < 2; ++pe) {
					for (ap_uint<4> d = 0; d < 4; ++d) {
						u_32_d_pe[2 * d    ][pe] = (u_64_pe_d[pe][d])(31,  0);
						u_32_d_pe[2 * d + 1][pe] = (u_64_pe_d[pe][d])(63, 32);
					}
				}

				for (ap_uint<4> d = 0; d < 8; ++d) {
					for (ap_uint<4> pe = 0; pe < 2; ++pe) {
						out_u0(31 + pe * 32 + d * 64, pe * 32 + d * 64) = u_32_d_pe[d + 0][pe];
					}
				}

				ap_uint<512> out_u0_mult;
				peg16mult(out_u0, alpha_u, out_u0_mult);
				fifo_C_out0.write(out_u0_mult);
			}
		}
	}
}

void C_collect(
    tapa::istreams<ap_uint<512>, NUM_CH_SPARSE> & fifo_C_in,
    tapa::ostreams<ap_uint<512>, NUM_CH_C> & fifo_C_out
    ) {
    ap_uint<512> tmp_c0;
    ap_uint<512> tmp_c1;
    ap_uint<512> tmp_c2;
    ap_uint<512> tmp_c3;
    ap_uint<512> tmp_c4;
    ap_uint<512> tmp_c5;
    ap_uint<512> tmp_c6;
    ap_uint<512> tmp_c7;

    bool c0_ready = false;
    bool c1_ready = false;
    bool c2_ready = false;
    bool c3_ready = false;
    bool c4_ready = false;
    bool c5_ready = false;
    bool c6_ready = false;
    bool c7_ready = false;

    ap_uint<512> MN512;
    bool M512_ready = false;
    w_Mxx: while(!M512_ready) {
#pragma HLS loop_tripcount min=1 max=10
#pragma HLS pipeline II=1
        M512_ready = fifo_C_in[7].try_read(MN512);
    };
    ap_uint<32> M = MN512(31, 0);
    ap_uint<32> P_N = MN512(63, 32);

    ap_uint<512> out_c0;
    ap_uint<512> out_c1;
    ap_uint<512> out_c2;
    ap_uint<512> out_c3;
    ap_uint<512> out_c4;
    ap_uint<512> out_c5;
    ap_uint<512> out_c6;
    ap_uint<512> out_c7;

    fifo_C_out[0].write(tapa::reg(MN512));
    fifo_C_out[1].write(tapa::reg(MN512));
    fifo_C_out[2].write(tapa::reg(MN512));
    fifo_C_out[3].write(tapa::reg(MN512));
    fifo_C_out[4].write(tapa::reg(MN512));
    fifo_C_out[5].write(tapa::reg(MN512));
    fifo_C_out[6].write(tapa::reg(MN512));
    fifo_C_out[7].write(tapa::reg(MN512));

    const ap_uint<16> N16 = P_N(31, 16);
    const ap_uint<16> rp_time = (N16 == 0)? ((ap_uint<16>) 1) : N16;
    const ap_uint<32> N = P_N & ((ap_uint<32>) 0x0000FFFF);
    const ap_uint<32> num_ite = ((M + 15) / 16) * ((N+7)/8);

    l_rp: for(ap_uint<16> rp = 0; rp < rp_time; rp++) {
#pragma HLS loop_flatten off
#pragma HLS loop_tripcount min=1 max=16
        cvt_b: for(ap_uint<32> i = 0; i < num_ite; ) {
#pragma HLS loop_tripcount min=1 max=800
#pragma HLS pipeline II=1
            if (!c0_ready) {
                c0_ready = fifo_C_in[0].try_read(tmp_c0);
            }
            if (!c1_ready) {
                c1_ready = fifo_C_in[1].try_read(tmp_c1);
            }
            if (!c2_ready) {
                c2_ready = fifo_C_in[2].try_read(tmp_c2);
            }
            if (!c3_ready) {
                c3_ready = fifo_C_in[3].try_read(tmp_c3);
            }
            if (!c4_ready) {
                c4_ready = fifo_C_in[4].try_read(tmp_c4);
            }
            if (!c5_ready) {
                c5_ready = fifo_C_in[5].try_read(tmp_c5);
            }
            if (!c6_ready) {
                c6_ready = fifo_C_in[6].try_read(tmp_c6);
            }
            if (!c7_ready) {
                c7_ready = fifo_C_in[7].try_read(tmp_c7);
            }

            bool all_c_ready = c0_ready && c1_ready && c2_ready && c3_ready && c4_ready && c5_ready && c6_ready && c7_ready;

            if (all_c_ready) {
                ap_uint<512> tmp_c0_delay = tapa::reg(tmp_c0);
                ap_uint<512> tmp_c1_delay = tapa::reg(tmp_c1);
                ap_uint<512> tmp_c2_delay = tapa::reg(tmp_c2);
                ap_uint<512> tmp_c3_delay = tapa::reg(tmp_c3);
                ap_uint<512> tmp_c4_delay = tapa::reg(tmp_c4);
                ap_uint<512> tmp_c5_delay = tapa::reg(tmp_c5);
                ap_uint<512> tmp_c6_delay = tapa::reg(tmp_c6);
                ap_uint<512> tmp_c7_delay = tapa::reg(tmp_c7);

                out_c0( 63,   0) = tmp_c0_delay(  63,    0);
                out_c0(127,  64) = tmp_c1_delay(  63,    0);
                out_c0(191, 128) = tmp_c2_delay(  63,    0);
                out_c0(255, 192) = tmp_c3_delay(  63,    0);
                out_c0(319, 256) = tmp_c4_delay(  63,    0);
                out_c0(383, 320) = tmp_c5_delay(  63,    0);
                out_c0(447, 384) = tmp_c6_delay(  63,    0);
                out_c0(511, 448) = tmp_c7_delay(  63,    0);

                out_c1( 63,   0) = tmp_c0_delay( 127,   64);
                out_c1(127,  64) = tmp_c1_delay( 127,   64);
                out_c1(191, 128) = tmp_c2_delay( 127,   64);
                out_c1(255, 192) = tmp_c3_delay( 127,   64);
                out_c1(319, 256) = tmp_c4_delay( 127,   64);
                out_c1(383, 320) = tmp_c5_delay( 127,   64);
                out_c1(447, 384) = tmp_c6_delay( 127,   64);
                out_c1(511, 448) = tmp_c7_delay( 127,   64);

                out_c2( 63,   0) = tmp_c0_delay( 191,  128);
                out_c2(127,  64) = tmp_c1_delay( 191,  128);
                out_c2(191, 128) = tmp_c2_delay( 191,  128);
                out_c2(255, 192) = tmp_c3_delay( 191,  128);
                out_c2(319, 256) = tmp_c4_delay( 191,  128);
                out_c2(383, 320) = tmp_c5_delay( 191,  128);
                out_c2(447, 384) = tmp_c6_delay( 191,  128);
                out_c2(511, 448) = tmp_c7_delay( 191,  128);

                out_c3( 63,   0) = tmp_c0_delay( 255,  192);
                out_c3(127,  64) = tmp_c1_delay( 255,  192);
                out_c3(191, 128) = tmp_c2_delay( 255,  192);
                out_c3(255, 192) = tmp_c3_delay( 255,  192);
                out_c3(319, 256) = tmp_c4_delay( 255,  192);
                out_c3(383, 320) = tmp_c5_delay( 255,  192);
                out_c3(447, 384) = tmp_c6_delay( 255,  192);
                out_c3(511, 448) = tmp_c7_delay( 255,  192);

                out_c4( 63,   0) = tmp_c0_delay( 319,  256);
                out_c4(127,  64) = tmp_c1_delay( 319,  256);
                out_c4(191, 128) = tmp_c2_delay( 319,  256);
                out_c4(255, 192) = tmp_c3_delay( 319,  256);
                out_c4(319, 256) = tmp_c4_delay( 319,  256);
                out_c4(383, 320) = tmp_c5_delay( 319,  256);
                out_c4(447, 384) = tmp_c6_delay( 319,  256);
                out_c4(511, 448) = tmp_c7_delay( 319,  256);

                out_c5( 63,   0) = tmp_c0_delay( 383,  320);
                out_c5(127,  64) = tmp_c1_delay( 383,  320);
                out_c5(191, 128) = tmp_c2_delay( 383,  320);
                out_c5(255, 192) = tmp_c3_delay( 383,  320);
                out_c5(319, 256) = tmp_c4_delay( 383,  320);
                out_c5(383, 320) = tmp_c5_delay( 383,  320);
                out_c5(447, 384) = tmp_c6_delay( 383,  320);
                out_c5(511, 448) = tmp_c7_delay( 383,  320);

                out_c6( 63,   0) = tmp_c0_delay( 447,  384);
                out_c6(127,  64) = tmp_c1_delay( 447,  384);
                out_c6(191, 128) = tmp_c2_delay( 447,  384);
                out_c6(255, 192) = tmp_c3_delay( 447,  384);
                out_c6(319, 256) = tmp_c4_delay( 447,  384);
                out_c6(383, 320) = tmp_c5_delay( 447,  384);
                out_c6(447, 384) = tmp_c6_delay( 447,  384);
                out_c6(511, 448) = tmp_c7_delay( 447,  384);

                out_c7( 63,   0) = tmp_c0_delay( 511,  448);
                out_c7(127,  64) = tmp_c1_delay( 511,  448);
                out_c7(191, 128) = tmp_c2_delay( 511,  448);
                out_c7(255, 192) = tmp_c3_delay( 511,  448);
                out_c7(319, 256) = tmp_c4_delay( 511,  448);
                out_c7(383, 320) = tmp_c5_delay( 511,  448);
                out_c7(447, 384) = tmp_c6_delay( 511,  448);
                out_c7(511, 448) = tmp_c7_delay( 511,  448);

                fifo_C_out[0].write(out_c0);
                fifo_C_out[1].write(out_c1);
                fifo_C_out[2].write(out_c2);
                fifo_C_out[3].write(out_c3);
                fifo_C_out[4].write(out_c4);
                fifo_C_out[5].write(out_c5);
                fifo_C_out[6].write(out_c6);
                fifo_C_out[7].write(out_c7);

                c0_ready = false;
                c1_ready = false;
                c2_ready = false;
                c3_ready = false;
                c4_ready = false;
                c5_ready = false;
                c6_ready = false;
                c7_ready = false;
                ++i;
            }
        }
    }
}

void write_C(
	tapa::istream<ap_uint<512>> & fifo_C,
	tapa::mmap<ap_uint<512>> C_out
	) {
	ap_uint<512> M_u512;
	bool M_ready = false;
	w_M: while(!M_ready) {
#pragma HLS loop_tripcount min=1 max=10
#pragma HLS pipeline II=1
		M_ready = fifo_C.try_read(M_u512);
	};
	ap_uint<32> M = M_u512(31, 0);
	ap_uint<32> P_N = M_u512(63, 32);

	const ap_uint<16> N16 = P_N(31, 16);
	const ap_uint<16> rp_time = (N16 == 0)? ((ap_uint<16>) 1) : N16;
	const ap_uint<32> N = P_N & ((ap_uint<32>) 0x0000FFFF);
	const ap_uint<32> num_ite_C = ((M + 15)/16) * ((N+7)/8);

	l_rp: for(ap_uint<16> rp = 0; rp < rp_time; rp++) {
#pragma HLS loop_flatten off
#pragma HLS loop_tripcount min=1 max=16
		wr_C: for(ap_uint<32> i = 0; i < num_ite_C; i++) {
#pragma HLS loop_tripcount min=1 max=500000
#pragma HLS pipeline II=1
			ap_uint<512> tmp_c = fifo_C.read();
			C_out[i] = tmp_c;
		}
	}
}

void read_C(
	tapa::mmap<ap_uint<512>> C,
	tapa::ostream<ap_uint<512>> & fifo_C,
	const ap_uint<32> M,
	const ap_uint<32> P_N
	) {
	const ap_uint<16> N16 = P_N(31, 16);
	const ap_uint<16> rp_time = (N16 == 0)? ((ap_uint<16>) 1) : N16;
	const ap_uint<32> N = P_N & ((ap_uint<32>) 0x0000FFFF);
	const ap_uint<32> num_ite_C = ((M + 15) / 16) * ((N+7)/8);

	l_rp: for(ap_uint<16> rp = 0; rp < rp_time; rp++) {
#pragma HLS loop_flatten off
#pragma HLS loop_tripcount min=1 max=16
		rd_C: for(ap_uint<32> i = 0; i < num_ite_C; i++) {
#pragma HLS loop_tripcount min=1 max=500000
#pragma HLS pipeline II=1
			ap_uint<512> tmp_c = C[i];
			fifo_C.write(tmp_c);
		}
	}
}

void comp_C(
	tapa::istream<ap_uint<512>> & fifo_C_read_in,
	tapa::istream<ap_uint<512>> & fifo_C_pe_in,
	tapa::ostream<ap_uint<512>> & fifo_C_out
	) {
	bool M_ready = false;
	ap_uint<512> M512;
	w_Mxx: while(!M_ready) {
#pragma HLS loop_tripcount min=1 max=10
#pragma HLS pipeline II=1
		M_ready = fifo_C_pe_in.try_read(M512);
	};
	fifo_C_out.write(M512);
	ap_uint<32> M = M512(31, 0);
	ap_uint<32> P_N = M512(63, 32);
	ap_uint<32> beta_u = M512(95, 64);

	float beta_f = tapa::bit_cast<float>(beta_u);

	ap_uint<512> c_out;
	ap_uint<512> c_read;
	ap_uint<512> c_pe;

	bool c_read_ready = false;
	bool c_pe_ready = false;

	const ap_uint<16> N16 = P_N(31, 16);
	const ap_uint<16> rp_time = (N16 == 0)? ((ap_uint<16>) 1) : N16;
	const ap_uint<32> N = P_N & ((ap_uint<32>) 0x0000FFFF);
	const ap_uint<32> num_ite_C = ((M + 15) / 16) * ((N+7)/8);

	l_rp: for(ap_uint<16> rp = 0; rp < rp_time; rp++) {
#pragma HLS loop_flatten off
#pragma HLS loop_tripcount min=1 max=16

		cc: for (ap_uint<32> i = 0; i < num_ite_C; ) {
#pragma HLS loop_tripcount min=1 max=5000
#pragma HLS pipeline II=1
			if (!c_read_ready) {
				c_read_ready = fifo_C_read_in.try_read(c_read);
			}
			if (!c_pe_ready) {
				c_pe_ready = fifo_C_pe_in.try_read(c_pe);
			}

			if (c_read_ready && c_pe_ready) {
				ap_uint<512> c_pe_delay = tapa::reg(c_pe);
				ap_uint<512> c_read_delay = tapa::reg(c_read);

				for(ap_uint<5> p = 0; p < 16; ++p) {
					float op_ab = tapa::bit_cast<float>(c_pe_delay(31 + p * 32, p * 32).to_uint());
					float op_c  = tapa::bit_cast<float>(c_read_delay(31 + p * 32, p * 32).to_uint());
					float op_result = op_ab + tapa::reg(beta_f) * op_c;
					c_out(31 + p * 32, p * 32) = tapa::bit_cast<ap_uint<32>>(op_result);
				}

				ap_uint<512> c_out_reg = tapa::reg(c_out);
				fifo_C_out.write(c_out_reg);
				c_read_ready = false;
				c_pe_ready = false;
				++i;
			}
		}
	}
}

constexpr int FIFO_DEPTH = 8;

void sextans(
	tapa::mmap<const ap_uint<32>> edge_list_ptr,

	tapa::mmaps<const ap_uint<512>, NUM_CH_SPARSE> edge_list_ch,

	tapa::mmaps<const ap_uint<512>, NUM_CH_B> mat_B_ch,

	tapa::mmaps<ap_uint<512>, NUM_CH_C> mat_C_ch_in,

	tapa::mmaps<ap_uint<512>, NUM_CH_C> mat_C_ch,

	const int NUM_ITE,
	const int NUM_A_LEN,
	const int M,
	const int K,
	const int P_N,
	const unsigned int alpha_u,
	const unsigned int beta_u
) {
	tapa::streams<ap_uint<32>, NUM_CH_SPARSE, FIFO_DEPTH> fifo_edge_list_ptr_pe("fifo_edge_list_ptr_pe");

	tapa::streams<ap_uint<512>, NUM_CH_SPARSE, FIFO_DEPTH> fifo_A_pe("fifo_A_pe");

	tapa::streams<ap_uint<512>, NUM_CH_SPARSE * NUM_CH_B, FIFO_DEPTH> fifo_B_pe_x("fifo_B_pe_x");

	tapa::streams<ap_uint<512>, NUM_CH_SPARSE, FIFO_DEPTH> fifo_C_pe("fifo_C_pe");

	tapa::streams<ap_uint<512>, NUM_CH_C, FIFO_DEPTH> fifo_C_read_in("fifo_C_read_in");

	tapa::streams<ap_uint<512>, NUM_CH_C, FIFO_DEPTH> fifo_C_ch_result("fifo_C_ch_result");

	tapa::streams<ap_uint<512>, NUM_CH_C, FIFO_DEPTH> fifo_C_ch("fifo_C_ch");

	tapa::task()
	.invoke(read_edge_list_ptr,
		NUM_ITE,
		M,
		P_N,
		K,
		alpha_u,
		beta_u,
		edge_list_ptr,
		fifo_edge_list_ptr_pe
		)

	.invoke<tapa::join, NUM_CH_SPARSE>(read_A,
		edge_list_ch,
		fifo_A_pe,
		NUM_A_LEN,
		P_N
		)

	.invoke<tapa::join, NUM_CH_B>(read_B,
		mat_B_ch,
		fifo_B_pe_x,
		K,
		P_N
		)

	.invoke<tapa::join, NUM_CH_SPARSE - 1>(PEG,
		fifo_edge_list_ptr_pe,
		fifo_A_pe,
		fifo_B_pe_x,
		fifo_edge_list_ptr_pe,
		fifo_B_pe_x,
		fifo_C_pe
		)

	.invoke(PEG_last,
		fifo_edge_list_ptr_pe,
		fifo_A_pe,
		fifo_B_pe_x,
		fifo_C_pe
		)

	.invoke(C_collect,
		fifo_C_pe,
		fifo_C_ch_result
		)

	.invoke<tapa::join, NUM_CH_C>(read_C,
		mat_C_ch_in,
		fifo_C_read_in,
		M,
		P_N
		)

	.invoke<tapa::join, NUM_CH_C>(comp_C,
		fifo_C_read_in,
		fifo_C_ch_result,
		fifo_C_ch
		)

	.invoke<tapa::join, NUM_CH_C>(write_C,
		fifo_C_ch,
		mat_C_ch
		)
	;
}
