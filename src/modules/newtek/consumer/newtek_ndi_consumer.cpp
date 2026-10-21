/*
 * Copyright 2018
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Krzysztof Zegzula, zegzulakrzysztof@gmail.com
 * based on work of Robert Nagy, ronag89@gmail.com
 */

#include "../StdAfx.h"

#include "newtek_ndi_consumer.h"
#include "ndi_box3.h"

#include <boost/thread/exceptions.hpp>
#include <chrono>
#include <condition_variable>
#include <core/consumer/channel_info.h>
#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/mixer/audio/audio_util.h>
#include <core/video_format.h>

#include <common/assert.h>
#include <common/diagnostics/graph.h>
#include <common/except.h>
#include <common/executor.h>
#include <common/future.h>
#include <common/param.h>
#include <common/timer.h>

#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/thread.hpp>
#include <ratio>
#include <thread>

#include "../util/ndi.h"
extern "C" {
#include <libswscale/swscale.h>
}

namespace caspar { namespace newtek {

struct newtek_ndi_consumer : public core::frame_consumer
{
    static std::atomic<int> instances_;
    const int               instance_no_;
    const std::wstring      name_;
    const bool              allow_fields_;

    core::video_format_desc              format_desc_;
    int                                  channel_index_;
    NDIlib_v5*                           ndi_lib_;
    NDIlib_video_frame_v2_t              ndi_video_frame_;
    NDIlib_audio_frame_interleaved_32s_t ndi_audio_frame_;
    std::shared_ptr<uint8_t>             field_data_;
    spl::shared_ptr<diagnostics::graph>  graph_;
    caspar::timer                        tick_timer_;
    caspar::timer                        frame_timer_;
    caspar::timer                        ndi_timer_;
    int                                  frame_no_;
    std::mutex                           buffer_mutex_;
    std::condition_variable              buffer_cond_;
    std::condition_variable              worker_cond_;
    bool                                 ready_for_frame_;
    std::queue<core::const_frame>        buffer_;
    bool stopping_ = false;
    int monitor_width_;
    int monitor_height_;
    SwsContext* scaler_ = nullptr;
    std::vector<uint8_t> scaled_;
    std::queue<std::vector<int32_t>> audio_buffer_;
    boost::thread audio_thread;
    boost::thread                        send_thread;
    executor                             executor_;

    std::unique_ptr<NDIlib_send_instance_t, std::function<void(NDIlib_send_instance_t*)>> ndi_send_instance_;

  public:
    newtek_ndi_consumer(std::wstring name, bool allow_fields, int width = 0, int height = 0)
        : name_(!name.empty() ? name : default_ndi_name())
        , instance_no_(instances_++)
        , frame_no_(0)
        , allow_fields_(allow_fields)
        , channel_index_(0)
        , monitor_width_(width)
        , monitor_height_(height)
        , executor_(L"ndi_consumer[" + std::to_wstring(instance_no_) + L"]")
    {
        ndi_lib_ = ndi::load_library();
        graph_->set_text(print());
        graph_->set_color("frame-time", diagnostics::color(0.5f, 1.0f, 0.2f));
        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("buffered-frames", diagnostics::color(0.5f, 0.0f, 0.2f));
        graph_->set_color("ndi-tick", diagnostics::color(1.0f, 1.0f, 0.1f));
        diagnostics::register_graph(graph_);
    }

    ~newtek_ndi_consumer()
    {
        if (send_thread.joinable()) {
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                stopping_ = true;
            }
            worker_cond_.notify_all();
            send_thread.join();
            if (audio_thread.joinable()) audio_thread.join();
            if (scaler_) sws_freeContext(scaler_);
        }
    }

    // frame_consumer

    void initialize(const core::video_format_desc& format_desc, const core::channel_info& channel_info, int port_index) override
    {
        format_desc_   = format_desc;
        channel_index_ = channel_info.index;

        NDIlib_send_create_t NDI_send_create_desc;

        auto tmp_name                   = u8(name_);
        NDI_send_create_desc.p_ndi_name = tmp_name.c_str();
        // NDI defaults to clocking on video, however it's very jittery.
        NDI_send_create_desc.clock_video = false;
        NDI_send_create_desc.clock_audio = false;

        ndi_send_instance_ = {new NDIlib_send_instance_t(ndi_lib_->send_create(&NDI_send_create_desc)),
                              [this](auto p) { this->ndi_lib_->send_destroy(*p); }};

        ndi_video_frame_.xres                 = format_desc.width;
        ndi_video_frame_.yres                 = format_desc.height;
        ndi_video_frame_.frame_rate_N         = format_desc.framerate.numerator() * format_desc.field_count;
        ndi_video_frame_.frame_rate_D         = format_desc.framerate.denominator();
        ndi_video_frame_.FourCC               = NDIlib_FourCC_type_BGRA;
        ndi_video_frame_.line_stride_in_bytes = format_desc.width * 4;
        ndi_video_frame_.frame_format_type    = NDIlib_frame_format_type_progressive;

        if (format_desc.field_count == 2 && allow_fields_) {
            ndi_video_frame_.yres /= 2;
            ndi_video_frame_.frame_rate_N /= 2;
            ndi_video_frame_.picture_aspect_ratio = format_desc.width * 1.0f / format_desc.height;
            field_data_.reset(new uint8_t[ndi_video_frame_.line_stride_in_bytes * ndi_video_frame_.yres],
                              std::default_delete<uint8_t[]>());
            ndi_video_frame_.p_data = field_data_.get();
        }

        if (monitor_width_ > 0 && monitor_height_ > 0 && format_desc.field_count == 1) {
            monitor_width_ = std::min(monitor_width_, format_desc.width);
            monitor_height_ = std::min(monitor_height_, format_desc.height);
            scaler_ = sws_getContext(format_desc.width, format_desc.height, AV_PIX_FMT_BGRA,
                                     monitor_width_, monitor_height_, AV_PIX_FMT_BGRA,
                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (!scaler_) CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("NDI scaler unavailable"));
            scaled_.resize(monitor_width_ * monitor_height_ * 4);
            ndi_video_frame_.xres = monitor_width_;
            ndi_video_frame_.yres = monitor_height_;
            ndi_video_frame_.line_stride_in_bytes = monitor_width_ * 4;
        }

        ndi_audio_frame_.sample_rate = format_desc_.audio_sample_rate;
        ndi_audio_frame_.no_channels = format_desc_.audio_channels;
        ndi_audio_frame_.timecode    = NDIlib_send_timecode_synthesize;

        graph_->set_text(print());
        // CASPAR_VERIFY(ndi_send_instance_);

        // Audio must not share the lossy confidence-video queue.
        audio_thread = boost::thread([this]() {
            set_thread_name(L"NDI-AUDIO: " + name_);
            while (true) {
                std::vector<int32_t> samples;
                {
                    std::unique_lock<std::mutex> lock(buffer_mutex_);
                    worker_cond_.wait(lock, [&] { return stopping_ || !audio_buffer_.empty(); });
                    if (stopping_) return;
                    samples = std::move(audio_buffer_.front());
                    audio_buffer_.pop();
                }
                ndi_audio_frame_.no_samples = static_cast<int>(samples.size()) / format_desc_.audio_channels;
                ndi_audio_frame_.p_data = samples.data();
                ndi_lib_->util_send_send_audio_interleaved_32s(*ndi_send_instance_, &ndi_audio_frame_);
            }
        });

        send_thread = boost::thread([=]() {
            set_thread_realtime_priority();
            set_thread_name(L"NDI-SEND: " + name_);
            CASPAR_LOG(info) << L"Starting ndi-send thread for ndi output: " << name_;
            try {
                auto buffer_size = buffer_.size();
                // Buffer a few frames to keep NDI going when caspar is slow on a few frames
                // This can be removed when CasparCG doesn't periodally slows down on frames
                while (!send_thread.interruption_requested()) {
                    {
                        std::unique_lock<std::mutex> lock(buffer_mutex_);
                        worker_cond_.wait(lock, [&] { return stopping_ || buffer_.size() >= 1; });
                        if (stopping_) return;
                        graph_->set_value("buffered-frames", static_cast<double>(buffer_.size() + 0.001) / 8);
                        buffer_size = buffer_.size();
                        if (buffer_.size() >= 1) {
                            break;
                        }
                    }
                }

                // Use steady clock to generate a near perfect NDI tick time.
                auto frametimeUs = static_cast<int>(1000000 / format_desc_.fps);
                auto time_point  = std::chrono::steady_clock::now();
                time_point += std::chrono::microseconds(frametimeUs);
                while (!send_thread.interruption_requested()) {
                    core::const_frame frame;
                    {
                        std::unique_lock<std::mutex> lock(buffer_mutex_);
                        worker_cond_.wait(lock, [&] { return stopping_ || !buffer_.empty(); });
                        if (stopping_) return;
                        graph_->set_value("buffered-frames", static_cast<double>(buffer_.size() + 0.001) / 8);
                        frame = std::move(buffer_.front());
                        buffer_.pop();
                    }
                    graph_->set_value("ndi-tick", ndi_timer_.elapsed() * format_desc_.fps * 0.5);
                    ndi_timer_.restart();
                    frame_timer_.restart();
                    if (format_desc_.field_count == 2 && allow_fields_) {
                        ndi_video_frame_.frame_format_type =
                            (frame_no_ % 2 ? NDIlib_frame_format_type_field_1 : NDIlib_frame_format_type_field_0);
                        for (auto y = 0; y < ndi_video_frame_.yres; ++y) {
                            std::memcpy(reinterpret_cast<char*>(ndi_video_frame_.p_data) + y * format_desc_.width * 4,
                                        frame.image_data(0).data() + (y * 2 + frame_no_ % 2) * format_desc_.width * 4,
                                        format_desc_.width * 4);
                        }
                    } else {
                        ndi_video_frame_.p_data = const_cast<uint8_t*>(frame.image_data(0).begin());
                    }
                    if (scaler_) {
                        const uint8_t* src[] = { frame.image_data(0).begin(), nullptr, nullptr, nullptr };
                        int src_stride[] = { format_desc_.width * 4, 0, 0, 0 };
                        uint8_t* dst[] = { scaled_.data(), nullptr, nullptr, nullptr };
                        int dst_stride[] = { monitor_width_ * 4, 0, 0, 0 };
                        if (format_desc_.width == monitor_width_ * 3 &&
                            format_desc_.height == monitor_height_ * 3) {
                            ndi_box3(reinterpret_cast<const uint32_t*>(src[0]),
                                     reinterpret_cast<uint32_t*>(dst[0]), monitor_width_, monitor_height_);
                        } else {
                            sws_scale(scaler_, src, src_stride, 0, format_desc_.height, dst, dst_stride);
                        }
                        ndi_video_frame_.p_data = scaled_.data();
                        // Release the shared GPU readback before NDI compression/network I/O.
                        frame = core::const_frame{};
                    }
                    ndi_lib_->send_send_video_v2(*ndi_send_instance_, &ndi_video_frame_);
                    frame_no_++;
                    graph_->set_value("frame-time", frame_timer_.elapsed() * format_desc_.fps * 0.5);
                    std::this_thread::sleep_until(time_point);
                    time_point += std::chrono::microseconds(frametimeUs);
                }
            } catch (boost::thread_interrupted) {
                // NOTHING
            }
        });
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        return executor_.begin_invoke([=] {
            graph_->set_value("tick-time", tick_timer_.elapsed() * format_desc_.fps * 0.5);
            tick_timer_.restart();
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (stopping_) return false;
                auto audio = frame.audio_data();
                if (audio.size() > 0) {
                    if (audio_buffer_.size() >= 256) {
                        audio_buffer_.pop();
                        CASPAR_LOG(warning) << L"NDI confidence audio queue exhausted";
                    }
                    audio_buffer_.emplace(audio.begin(), audio.end());
                }
                // Confidence output must never accumulate unlimited UHD frames.
                // Dropping the oldest monitor frame cannot stall the program.
                while (buffer_.size() >= 1) buffer_.pop();
                buffer_.push(std::move(frame));
            }
            worker_cond_.notify_all();
            return true;
        });
    }

    std::wstring print() const override
    {
        if (channel_index_) {
            return L"ndi_consumer[" + boost::lexical_cast<std::wstring>(channel_index_) + L"|" + name_ + L"]";
        } else {
            return L"[ndi_consumer]";
        }
    }

    std::wstring name() const override { return L"ndi"; }

    std::wstring default_ndi_name() const
    {
        return L"CasparCG" + (instance_no_ ? L" " + boost::lexical_cast<std::wstring>(instance_no_) : L"");
    }

    int index() const override { return 900; }

    bool has_synchronization_clock() const override { return false; }

    core::monitor::state state() const override
    {
        core::monitor::state state;
        state["ndi/name"]         = name_;
        state["ndi/allow_fields"] = allow_fields_;
        return state;
    }
};

std::atomic<int> newtek_ndi_consumer::instances_(0);

spl::shared_ptr<core::frame_consumer>
create_ndi_consumer(const std::vector<std::wstring>&                         params,
                    const core::video_format_repository&                     format_repository,
                    const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                    const core::channel_info&                                channel_info)
{
    if (params.size() < 1 || !boost::iequals(params.at(0), L"NDI"))
        return core::frame_consumer::empty();

    if (channel_info.depth != common::bit_depth::bit8)
        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Newtek NDI consumer only supports 8-bit color depth."));

    std::wstring name         = get_param(L"NAME", params, L"");
    bool         allow_fields = contains_param(L"ALLOW_FIELDS", params);
    return spl::make_shared<newtek_ndi_consumer>(name, allow_fields, get_param(L"WIDTH", params, 0), get_param(L"HEIGHT", params, 0));
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_ndi_consumer(const boost::property_tree::wptree&                      ptree,
                                  const core::video_format_repository&                     format_repository,
                                  const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                  const core::channel_info&                                channel_info)
{
    auto name         = ptree.get(L"name", L"");
    bool allow_fields = ptree.get(L"allow-fields", false);

    if (channel_info.depth != common::bit_depth::bit8)
        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Newtek NDI consumer only supports 8-bit color depth."));

    return spl::make_shared<newtek_ndi_consumer>(name, allow_fields, ptree.get(L"width", 0), ptree.get(L"height", 0));
}

}} // namespace caspar::newtek
