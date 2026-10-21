// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

namespace caspar { namespace ffmpeg {
// Prefixes end at a path boundary. Other protocols and empty prefixes are never cameras.
inline bool camera_prefix_matches(const std::string& path, const std::string& prefix)
{
    const bool network = prefix.rfind("rtsp://", 0) == 0 || prefix.rfind("rtmp://", 0) == 0 || prefix.rfind("rtmps://", 0) == 0;
    const auto authority = prefix.find("://");
    const auto slash = authority == std::string::npos ? std::string::npos : prefix.find('/', authority + 3);
    return network && slash != std::string::npos && slash > authority + 3 &&
           prefix.back() == '/' && path.size() > prefix.size() && path.rfind(prefix, 0) == 0;
}
}}
