/*******************************************************************************
* Copyright 2016-2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <assert.h>
#include <math.h>

#include "c_types_map.hpp"
#include "type_helpers.hpp"

#include "ref_batch_normalization.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

template <impl::data_type_t data_type>
void ref_batch_normalization_fwd_t<data_type>::execute_forward() {
    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    /* FIXME: check this */
    data_t* mean = conf_.stats_is_src() ?
        const_cast<data_t*>(reinterpret_cast<const data_t*>(
               this->input_memory(1))) :
        reinterpret_cast<data_t*>(this->memory(1));

    data_t* variance = conf_.stats_is_src() ?
        const_cast<data_t*>(reinterpret_cast<const data_t*>(
                this->input_memory(2))) :
        reinterpret_cast<data_t*>(this->memory(2));

    auto idx_scaleshift = 1 + 2*conf_.stats_is_src();
    auto scaleshift =
        reinterpret_cast<const data_t *>(this->input_memory(idx_scaleshift));

    auto dst = reinterpret_cast<data_t*>(this->memory(0));
    auto ws = reinterpret_cast<uint8_t *>(this->memory(conf_.ws_idx()));

    const memory_desc_wrapper data_d(conf_.src_pd());
    const memory_desc_wrapper scaleshift_d(conf_.weights_pd());

    const int N = conf_.MB();
    const int C = conf_.C();
    const int H = conf_.H();
    const int W = conf_.W();

    const float eps = conf_.desc()->batch_norm_epsilon;
    const bool use_scaleshift = conf_.use_scaleshift();;
    const bool save_stats = conf_.is_training();
    const bool calculate_stats = !conf_.stats_is_src();

    const bool with_relu = conf_.with_relu_post_op();
    auto maybe_post_op = [&](data_t res) {
        return (with_relu && res < 0) ? 0 : res;
    };

#   pragma omp parallel for schedule(static)
    for (int c = 0; c < C; ++c) {
        data_t v_mean = calculate_stats ? 0 : mean[c];
        data_t v_variance = calculate_stats ? 0 : variance[c];

        data_t sm = use_scaleshift ? scaleshift[scaleshift_d.off(0, c)] : 1;
        data_t sv = use_scaleshift ? scaleshift[scaleshift_d.off(1, c)] : 0;

        if (calculate_stats) {
            for (int n = 0; n < N; ++n)
            for (int h = 0; h < H; ++h)
            for (int w = 0; w < W; ++w)
                v_mean += src[data_d.off(n, c, h, w)];
            v_mean /= W*N*H;

            for (int n = 0; n < N; ++n)
            for (int h = 0; h < H; ++h)
            for (int w = 0; w < W; ++w) {
                data_t m = src[data_d.off(n,c,h,w)] - v_mean;
                v_variance += m*m;
            }
            v_variance /= W*H*N;
        }
        data_t sqrt_variance =
            static_cast<data_t>(1. / sqrt(v_variance + eps));

        for (int n = 0; n < N; ++n)
        for (int h = 0; h < H; ++h)
        for (int w = 0; w < W; ++w) {
            auto d_off = data_d.off(n,c,h,w);
            data_t bn_res = sm * (src[d_off] - v_mean) * sqrt_variance + sv;
            if (conf_.fuse_bn_relu()) {
                if (bn_res <= 0) {
                    bn_res = 0;
                    if (ws) ws[d_off] = 0;
                } else {
                    if (ws) ws[d_off] = 1;
                }
            }
            dst[d_off] = maybe_post_op(bn_res);
        }

        if (calculate_stats) {
            if (save_stats) {
                mean[c] = v_mean;
                variance[c] = v_variance;
            }
        }
    }
}

template struct ref_batch_normalization_fwd_t<data_type::f32>;

template <impl::data_type_t data_type>
void ref_batch_normalization_bwd_t<data_type>::execute_backward() {
    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto mean = reinterpret_cast<const data_t *>(this->input_memory(1));
    auto variance = reinterpret_cast<const data_t *>(this->input_memory(2));
    auto diff_dst = reinterpret_cast<const data_t *>(this->input_memory(3));
    auto scaleshift = reinterpret_cast<const data_t *>(this->input_memory(4));
    auto ws = reinterpret_cast<const uint8_t *>(
            this->input_memory(conf_.ws_idx()));

    auto diff_src = reinterpret_cast<data_t*>(this->memory(0));
    auto diff_scaleshift = reinterpret_cast<data_t *>(this->memory(1));

    const memory_desc_wrapper data_d(conf_.src_pd());
    const memory_desc_wrapper diff_data_d(conf_.diff_src_pd());
    const memory_desc_wrapper scaleshift_d(conf_.weights_pd());
    const memory_desc_wrapper diff_scaleshift_d(conf_.diff_weights_pd());
    const memory_desc_wrapper mean_d(conf_.mean_pd());
    const memory_desc_wrapper variance_d(conf_.variance_pd());

    const int N = conf_.MB();
    const int C = conf_.C();
    const int H = conf_.H();
    const int W = conf_.W();

    const float eps = conf_.desc()->batch_norm_epsilon;
    const bool use_scaleshift = conf_.use_scaleshift();
    const bool calculate_diff_stats = !conf_.omit_stats();

#   pragma omp parallel for schedule(static)
    for (int c = 0; c < C; ++c) {
        data_t v_mean = mean[mean_d.off(c)];
        data_t v_variance = variance[variance_d.off(c)];
        data_t sqrt_variance = static_cast<data_t>(1. / sqrt(v_variance + eps));
        data_t gamma = use_scaleshift ? scaleshift[scaleshift_d.off(0, c)] : 1;
        data_t diff_gamma = data_t(0);
        data_t diff_beta = data_t(0);
        diff_gamma = 0.0;
        diff_beta = 0.0;

        for (int n = 0; n < N; ++n)
        for (int h = 0; h < H; ++h)
        for (int w = 0; w < W; ++w) {
            const auto s_off = data_d.off(n, c, h, w);
            data_t dd = diff_dst[diff_data_d.off(n, c, h, w)];
            if (ws && !ws[s_off]) dd = 0;

            diff_gamma += (src[s_off] - v_mean) * dd;
            diff_beta += dd;
        }
        diff_gamma *= sqrt_variance;

        if (diff_scaleshift) {
            diff_scaleshift[diff_scaleshift_d.off(0, c)] = diff_gamma;
            diff_scaleshift[diff_scaleshift_d.off(1, c)] = diff_beta;
        }

        for (int n = 0; n < N; ++n)
        for (int h = 0; h < H; ++h)
        for (int w = 0; w < W; ++w) {
            const size_t s_off = data_d.off(n, c, h, w);
            const size_t dd_off = diff_data_d.off(n, c, h, w);
            data_t dd = diff_dst[dd_off];
            if (ws && !ws[s_off]) dd = 0;

            data_t v_diff_src = dd;
            if (calculate_diff_stats) {
                v_diff_src -= diff_beta/(W*H*N) +
                    (src[s_off] - v_mean) *
                    diff_gamma*sqrt_variance/(W*H*N);
            }
            v_diff_src *= gamma*sqrt_variance;
            diff_src[dd_off] = v_diff_src;
        }
    }
}

template struct ref_batch_normalization_bwd_t<data_type::f32>;

}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
