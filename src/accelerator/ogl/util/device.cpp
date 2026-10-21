/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
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
 * Author: Robert Nagy, ronag89@gmail.com
 */
#include "device.h"

#include "buffer.h"
#include "context.h"
#include "shader.h"
#include "texture.h"
#include "idle_cache.h"
#include <tuple>

#include <common/array.h>
#include <common/assert.h>
#include <common/env.h>
#include <common/except.h>
#include <common/gl/gl_check.h>
#include <common/os/thread.h>

#include <GL/glew.h>

#ifdef WIN32
#include "../../d3d/d3d_device.h"
#include <GL/wglew.h>
#endif

#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/property_tree/ptree.hpp>

#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_unordered_map.h>

#include <array>
#include <atomic>
#include <future>
#include <thread>

namespace caspar { namespace accelerator { namespace ogl {

using namespace boost::asio;

struct device::impl : public std::enable_shared_from_this<impl>
{
    std::unique_ptr<device_context> context_;
    static constexpr size_t cache_budget_ = 384ULL * 1024 * 1024;
    using texture_key = std::tuple<int, int, int, int>;
    idle_cache<texture_key, std::shared_ptr<texture>> device_cache_{cache_budget_};
    idle_cache<std::pair<bool, int>, std::shared_ptr<buffer>> host_cache_{cache_budget_};

    GLuint fbo_;

    std::wstring version_;

#ifdef WIN32
    std::shared_ptr<d3d::d3d_device> d3d_device_;
    std::shared_ptr<void>            interop_handle_;
#endif

    io_context                             io_context_;
    decltype(make_work_guard(io_context_)) work_;
    std::thread                            thread_;

    impl()
        : context_(new device_context())
        , work_(make_work_guard(io_context_))
    {
        CASPAR_LOG(info) << L"Initializing OpenGL Device.";

        context_->bind();

        auto err = glewInit();
        if (err != GLEW_OK && err != 4) { // GLEW_ERROR_NO_GLX_DISPLAY
            std::stringstream str;
            str << "Failed to initialize GLEW (" << (int)err << "): " << glewGetErrorString(err) << std::endl;
            CASPAR_THROW_EXCEPTION(gl::ogl_exception() << msg_info(str.str()));
        }

#ifdef WIN32
        if (wglewInit() != GLEW_OK) {
            CASPAR_THROW_EXCEPTION(gl::ogl_exception() << msg_info("Failed to initialize GLEW."));
        }
#endif

        version_ = u16(reinterpret_cast<const char*>(GL2(glGetString(GL_VERSION)))) + L" " +
                   u16(reinterpret_cast<const char*>(GL2(glGetString(GL_VENDOR))));

        CASPAR_LOG(info) << L"Initialized OpenGL " << version();

        if (!GLEW_VERSION_4_5 && !glewIsSupported("GL_ARB_sync GL_ARB_shader_objects GL_ARB_multitexture "
                                                  "GL_ARB_direct_state_access GL_ARB_texture_barrier")) {
            CASPAR_THROW_EXCEPTION(not_supported()
                                   << msg_info("Your graphics card does not meet the minimum hardware requirements "
                                               "since it does not support OpenGL 4.5 or higher."));
        }

        GL(glCreateFramebuffers(1, &fbo_));
        GL(glBindFramebuffer(GL_FRAMEBUFFER, fbo_));

        context_->unbind();

#ifdef WIN32
        if (env::properties().get(L"configuration.html.enable-gpu", false)) {
            d3d_device_ = d3d::d3d_device::get_device();
        }
        if (d3d_device_) {
            interop_handle_ = std::shared_ptr<void>(wglDXOpenDeviceNV(d3d_device_->device()), [](void* p) {
                if (p)
                    wglDXCloseDeviceNV(p);
            });

            if (!interop_handle_)
                CASPAR_THROW_EXCEPTION(gl::ogl_exception() << msg_info("Failed to initialize d3d interop."));
        }
#endif

        thread_ = std::thread([&] {
            context_->bind();
            set_thread_name(L"OpenGL Device");
            io_context_.run();
            context_->unbind();
        });
    }

    ~impl()
    {
        work_.reset();
        thread_.join();

        context_->bind();

        host_cache_.clear();
        device_cache_.clear();

        GL(glDeleteFramebuffers(1, &fbo_));
    }

    template <typename Func>
    auto spawn_async(Func&& func)
    {
        using result_type = decltype(func(std::declval<yield_context>()));
        using task_type   = std::packaged_task<result_type(yield_context)>;

        auto task   = task_type(std::forward<Func>(func));
        auto future = task.get_future();
        boost::asio::spawn(io_context_,
                           std::move(task)
#if BOOST_VERSION >= 108000
                               ,
                           [](std::exception_ptr e) {
                               if (e)
                                   std::rethrow_exception(e);
                           }
#endif
        );
        return future;
    }

    template <typename Func>
    auto dispatch_async(Func&& func)
    {
        using result_type = decltype(func());
        using task_type   = std::packaged_task<result_type()>;

        auto task   = task_type(std::forward<Func>(func));
        auto future = task.get_future();
        boost::asio::dispatch(io_context_, std::move(task));
        return future;
    }

    template <typename Func>
    auto dispatch_sync(Func&& func) -> decltype(func())
    {
        return dispatch_async(std::forward<Func>(func)).get();
    }

    std::wstring version() { return version_; }

    std::shared_ptr<texture> create_texture(int width, int height, int stride, common::bit_depth depth, bool clear)
    {
        CASPAR_VERIFY(stride > 0 && stride < 5);
        CASPAR_VERIFY(width > 0 && height > 0);

        auto depth_pool_index = depth == common::bit_depth::bit8 ? 0 : 1;

        const texture_key key{depth_pool_index, width, height, stride};
        auto tex = device_cache_.take(key);
        if (!tex) tex = std::make_shared<texture>(width, height, stride, depth);
        tex->set_depth(depth);

        if (clear) {
            tex->clear();
        }

        auto ptr = tex.get();
        return std::shared_ptr<texture>(
            ptr, [tex = std::move(tex), key, self = shared_from_this()](texture*) mutable {
                const auto bytes = tex->size();
                auto retired = self->device_cache_.put(key, std::move(tex), bytes);
                if (!retired.empty())
                    self->dispatch_async([retired = std::move(retired)]() mutable { retired.clear(); });
            });
    }

    std::shared_ptr<buffer> create_buffer(int size, bool write)
    {
        CASPAR_VERIFY(size > 0);
        const auto key = std::make_pair(write, size);
        auto buf = host_cache_.take(key);
        if (!buf) dispatch_sync([&] { buf = std::make_shared<buffer>(size, write); });
        auto ptr = buf.get();
        return std::shared_ptr<buffer>(ptr, [buf = std::move(buf), key, self = shared_from_this()](buffer*) mutable {
            const auto bytes = buf->size();
            auto retired = self->host_cache_.put(key, std::move(buf), bytes);
            if (!retired.empty())
                self->dispatch_async([retired = std::move(retired)]() mutable { retired.clear(); });
        });
    }

    array<uint8_t> create_array(int size)
    {
        auto buf = create_buffer(size, true);
        auto ptr = reinterpret_cast<uint8_t*>(buf->data());
        return array<uint8_t>(ptr, buf->size(), std::move(buf));
    }

    std::future<std::shared_ptr<texture>>
    copy_async(const array<const uint8_t>& source, int width, int height, int stride, common::bit_depth depth)
    {
        return dispatch_async([=] {
            std::shared_ptr<buffer> buf;

            auto tmp = source.storage<std::shared_ptr<buffer>>();
            if (tmp) {
                buf = *tmp;
            } else {
                buf = create_buffer(static_cast<int>(source.size()), true);
                // TODO (perf) Copy inside a TBB worker.
                std::memcpy(buf->data(), source.data(), source.size());
            }

            auto tex = create_texture(width, height, stride, depth, false);
            tex->copy_from(*buf);
            // TODO (perf) save tex on source
            return tex;
        });
    }

    std::future<array<const uint8_t>> copy_async(const std::shared_ptr<texture>& source)
    {
        return spawn_async([=](yield_context yield) {
            auto buf = create_buffer(source->size(), false);
            source->copy_to(*buf);

            auto fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

            GL(glFlush());

            deadline_timer timer(io_context_);
            for (auto n = 0; true; ++n) {
                // TODO (perf) Smarter non-polling solution?
                timer.expires_from_now(boost::posix_time::milliseconds(2));
                timer.async_wait(yield);

                auto wait = glClientWaitSync(fence, 0, 1);
                if (wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED) {
                    break;
                }
            }

            glDeleteSync(fence);

            auto ptr  = reinterpret_cast<uint8_t*>(buf->data());
            auto size = buf->size();
            return array<const uint8_t>(ptr, size, std::move(buf));
        });
    }

#ifdef WIN32
    std::future<std::shared_ptr<texture>>
    copy_async(GLuint source, int width, int height, int stride, common::bit_depth depth)
    {
        return spawn_async([=](yield_context yield) {
            auto tex = create_texture(width, height, stride, depth, false);

            tex->copy_from(source);

            auto fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

            GL(glFlush());

            deadline_timer timer(io_context_);
            for (auto n = 0; true; ++n) {
                // TODO (perf) Smarter non-polling solution?
                timer.expires_from_now(boost::posix_time::milliseconds(2));
                timer.async_wait(yield);

                auto wait = glClientWaitSync(fence, 0, 1);
                if (wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED) {
                    break;
                }
            }

            glDeleteSync(fence);

            return tex;
        });
    }
#endif

    boost::property_tree::wptree info() const
    {
        boost::property_tree::wptree info;

        boost::property_tree::wptree pooled_device_buffers;
        size_t                       total_pooled_device_buffer_size  = 0;
        size_t                       total_pooled_device_buffer_count = 0;

        const auto device_status = device_cache_.status();
        for (const auto& item : device_status.groups) {
            const auto width = std::get<1>(item.first);
            const auto height = std::get<2>(item.first);
            const auto stride = std::get<3>(item.first);
            const auto count = item.second.first;
            const auto bytes = item.second.second;
            boost::property_tree::wptree pool_info;
            pool_info.add(L"stride", stride);
            pool_info.add(L"mipmapping", false);
            pool_info.add(L"width", width);
            pool_info.add(L"height", height);
            pool_info.add(L"size", bytes / count);
            pool_info.add(L"count", count);
            total_pooled_device_buffer_size += bytes;
            total_pooled_device_buffer_count += count;
            pooled_device_buffers.add_child(L"device_buffer_pool", pool_info);
        }

        info.add_child(L"gl.details.pooled_device_buffers", pooled_device_buffers);

        boost::property_tree::wptree pooled_host_buffers;
        size_t                       total_read_size   = 0;
        size_t                       total_write_size  = 0;
        size_t                       total_read_count  = 0;
        size_t                       total_write_count = 0;

        const auto host_status = host_cache_.status();
        for (const auto& item : host_status.groups) {
            const auto is_write = item.first.first;
            const auto size = item.first.second;
            const auto count = item.second.first;
            boost::property_tree::wptree pool_info;
            pool_info.add(L"usage", is_write ? L"write_only" : L"read_only");
            pool_info.add(L"size", size);
            pool_info.add(L"count", count);
            pooled_host_buffers.add_child(L"host_buffer_pool", pool_info);
            (is_write ? total_write_count : total_read_count) += count;
            (is_write ? total_write_size : total_read_size) += item.second.second;
        }

        info.add_child(L"gl.details.pooled_host_buffers", pooled_host_buffers);
        info.add(L"gl.summary.pooled_device_buffers.total_count", total_pooled_device_buffer_count);
        info.add(L"gl.summary.pooled_device_buffers.total_size", total_pooled_device_buffer_size);
        // info.add_child(L"gl.summary.all_device_buffers", texture::info());
        info.add(L"gl.summary.pooled_host_buffers.total_read_count", total_read_count);
        info.add(L"gl.summary.pooled_host_buffers.total_write_count", total_write_count);
        info.add(L"gl.summary.pooled_host_buffers.total_read_size", total_read_size);
        info.add(L"gl.summary.pooled_host_buffers.total_write_size", total_write_size);
        info.add_child(L"gl.summary.all_host_buffers", buffer::info());
        info.add(L"gl.summary.cache_budget_bytes", cache_budget_);
        info.add(L"gl.summary.cached_device_bytes", device_status.bytes);
        info.add(L"gl.summary.cached_host_bytes", host_status.bytes);

        info.add(L"gl.summary.cache_policy", L"oldest-idle-first");
        info.add(L"gl.summary.device_evictions", device_status.evictions);
        info.add(L"gl.summary.host_evictions", host_status.evictions);
        return info;
    }

    std::future<void> gc()
    {
        return spawn_async([=](yield_context yield) {
            CASPAR_LOG(info) << " ogl: Running GC.";

            try {
                device_cache_.clear();
                host_cache_.clear();
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        });
    }
};

device::device()
    : impl_(new impl())
{
}
device::~device() {}
std::shared_ptr<texture> device::create_texture(int width, int height, int stride, common::bit_depth depth)
{
    return impl_->create_texture(width, height, stride, depth, true);
}
array<uint8_t> device::create_array(int size) { return impl_->create_array(size); }
std::future<std::shared_ptr<texture>>
device::copy_async(const array<const uint8_t>& source, int width, int height, int stride, common::bit_depth depth)
{
    return impl_->copy_async(source, width, height, stride, depth);
}
std::future<array<const uint8_t>> device::copy_async(const std::shared_ptr<texture>& source)
{
    return impl_->copy_async(source);
}

#ifdef WIN32
std::shared_ptr<void> device::d3d_interop() const { return impl_->interop_handle_; }
std::future<std::shared_ptr<texture>>
device::copy_async(GLuint source, int width, int height, int stride, common::bit_depth depth)
{
    return impl_->copy_async(source, width, height, stride, depth);
}
#endif

void device::dispatch(std::function<void()> func) { boost::asio::dispatch(impl_->io_context_, std::move(func)); }
std::wstring                 device::version() const { return impl_->version(); }
boost::property_tree::wptree device::info() const { return impl_->info(); }
std::future<void>            device::gc() { return impl_->gc(); }
}}} // namespace caspar::accelerator::ogl
