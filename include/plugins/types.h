// Copyright (c) 2023 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#ifndef CURAENGINE_INCLUDE_PLUGINS_TYPES_H
#define CURAENGINE_INCLUDE_PLUGINS_TYPES_H

#include <Arcus/Types.h>
#include <memory>
#include <tuple>

#include "utils/IntPoint.h"
#include "utils/polygon.h"

#include "plugin.grpc.pb.h"

namespace cura::plugins
{
using SlotID = proto::SlotID;

namespace details
{
template<size_t N>
struct CharRangeLiteral
{
    constexpr CharRangeLiteral(const char (&str)[N])
    {
        std::copy_n(str, N, value);
    }

    char value[N];
};

} // namespace details

namespace converters
{

template<typename C, typename M, typename R>
concept ReceiveCallable = requires(C callable, M message)
{
    { callable(message) } -> std::same_as<R>;
    std::is_base_of_v<google::protobuf::Message, M>;
};

template<typename C, typename M, typename... S>
concept SendCallable = requires(C callable, S... args)
{
    { callable(args...) } -> std::same_as<std::shared_ptr<M>>;
    std::is_base_of_v<google::protobuf::Message, M>;
};

const auto receive_slot_id
{
    [](const proto::PluginResponse& message)
    {
        return std::tuple<std::string, std::string>{ message.version(), message.plugin_hash() };
    }
};
static_assert(ReceiveCallable<decltype(receive_slot_id), proto::PluginResponse, std::tuple<std::string, std::string>>);

const auto send_slot_id
{
    [](const cura::plugins::proto::SlotID& slot_id)
    {
        proto::PluginRequest message{};
        message.set_id(slot_id);
        return std::make_shared<proto::PluginRequest>(message);
    }
};
static_assert(SendCallable<decltype(send_slot_id), proto::PluginRequest, cura::plugins::proto::SlotID>);

const auto receive_simplify
{
    [](const proto::SimplifyResponse& message)
    {
        Polygons poly{};
        for (const auto& paths : message.polygons().paths())
        {
            Polygon p{};
            for (const auto& point : paths.path())
            {
                p.add(Point{ point.y(), point.y() });
            }
            poly.add(p);
        }
        return poly;
    }
};
static_assert(ReceiveCallable<decltype(receive_simplify), proto::SimplifyResponse, Polygons>);

const auto send_simplify
{
    [](const Polygons& polygons, const size_t max_deviation, const size_t max_angle)
    {
        proto::SimplifyRequest message{};
        message.set_max_deviation(max_deviation);
        message.set_max_angle(max_angle);
        for (const auto& polygon : polygons.paths)
        {
            auto poly = message.polygons();
            for (const auto& path : polygons.paths)
            {
                auto* p = poly.add_paths();
                for (const auto& point : path)
                {
                    auto* pt = p->add_path();
                    pt->set_x(point.X);
                    pt->set_y(point.Y);
                }
            }
        }
        return std::make_shared<proto::SimplifyRequest>(message);
    }
};
static_assert(SendCallable<decltype(send_simplify), proto::SimplifyRequest, Polygons, size_t, size_t>);

const auto receive_postprocess
{
    [](const proto::PostprocessResponse& message)
    {
        return message.gcode_word();
    }
};
static_assert(ReceiveCallable<decltype(receive_postprocess), proto::PostprocessResponse, std::string>);

const auto send_postprocess
{
    [](const std::string& gcode)
    {
        proto::PostprocessRequest message{};
        message.set_gcode_word(gcode);
        return std::make_shared<proto::PostprocessRequest>(message);
    }
};
static_assert(SendCallable<decltype(send_postprocess), proto::PostprocessRequest, std::string>);

} // namespace converters

} // namespace cura::plugins

#endif // CURAENGINE_INCLUDE_PLUGINS_TYPES_H