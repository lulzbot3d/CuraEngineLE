// Copyright (c) 2024 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#ifdef ARCUS

#include "communication/ArcusCommunication.h"

#ifdef SENTRY_URL
#ifdef _WIN32
#if ! defined(NOMINMAX)
#define NOMINMAX
#endif
#if ! defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <sentry.h>
#endif

#include <thread> //To sleep while waiting for the connection.
#include <unordered_map> //To map settings to their extruder numbers for limit_to_extruder.

#include <Arcus/Socket.h> //The socket to communicate to.
#include <fmt/format.h>
#include <spdlog/details/os.h>
#include <spdlog/spdlog.h>

#include "Application.h" //To get and set the current slice command.
#include "ExtruderTrain.h"
#include "FffProcessor.h" //To start a slice.
#include "PrintFeature.h"
#include "Slice.h" //To process slices.
#include "communication/ArcusCommunicationPrivate.h" //Our PIMPL.
#include "communication/Listener.h" //To listen to the Arcus socket.
#include "communication/SliceDataStruct.h" //To store sliced layer data.
#include "geometry/Polygon.h"
#include "plugins/slots.h"
#include "settings/types/LayerIndex.h" //To point to layers.
#include "settings/types/Velocity.h" //To send to layer view how fast stuff is printing.
#include "utils/channel.h"

namespace cura
{

/*
 * \brief A computation class that formats layer view data in a way that the
 * front-end can understand it.
 *
 * This converts data from CuraEngine's internal data structures to Protobuf
 * messages that can be sent to the front-end.
 */
class ArcusCommunication::PathCompiler
{
    typedef cura::proto::PathSegment::PointType PointType;
    static_assert(sizeof(PrintFeatureType) == 1, "To be compatible with the Cura frontend code PrintFeatureType needs to be of size 1");
    //! Reference to the private data of the CommandSocket used to send the data to the front end.
    ArcusCommunication::Private& _cs_private_data;
    //! Keeps track of the current layer number being processed. If layer number is set to a different value, the current data is flushed to CommandSocket.
    LayerIndex _layer_nr;
    size_t extruder;
    PointType data_point_type;

    std::vector<PrintFeatureType> line_types; //!< Line types for the line segments stored, the size of this vector is N.
    std::vector<float> line_widths; //!< Line widths for the line segments stored, the size of this vector is N.
    std::vector<float> line_thicknesses; //!< Line thicknesses for the line segments stored, the size of this vector is N.
    std::vector<float> line_velocities; //!< Line feedrates for the line segments stored, the size of this vector is N.
    std::vector<float> points; //!< The points used to define the line segments, the size of this vector is D*(N+1) as each line segment is defined from one point to the next. D is
                               //!< the dimensionality of the point.

    Point3LL last_point;

    PathCompiler(const PathCompiler&) = delete;
    PathCompiler& operator=(const PathCompiler&) = delete;

public:
    /*
     * Create a new path compiler.
     */
    PathCompiler(ArcusCommunication::Private& cs_private_data)
        : _cs_private_data(cs_private_data)
        , _layer_nr(0)
        , extruder(0)
        , data_point_type(cura::proto::PathSegment::Point3D)
        , line_types()
        , line_widths()
        , line_thicknesses()
        , line_velocities()
        , points()
    {
    }

    /*
     * Flush the remaining unflushed paths when destroying this compiler.
     */
    ~PathCompiler()
    {
        if (line_types.size())
        {
            flushPathSegments();
        }
    }

    /*!
     * \brief Used to select which layer the following layer data is intended
     * for.
     * \param new_layer_nr The new layer to switch to.
     */
    void setLayer(const LayerIndex& new_layer_nr)
    {
        if (_layer_nr != new_layer_nr)
        {
            flushPathSegments();
            _layer_nr = new_layer_nr;
        }
    }

    /*!
     * \brief Returns the current layer which data is written to.
     */
    int getLayer() const
    {
        return _layer_nr;
    }
    /*!
     * \brief Used to set which extruder will be used for printing the following
     * layer data.
     * \param new_extruder The new extruder to switch to.
     */
    void setExtruder(const ExtruderTrain& new_extruder)
    {
        if (extruder != new_extruder.extruder_nr_)
        {
            flushPathSegments();
            extruder = new_extruder.extruder_nr_;
        }
    }

    /*!
     * \brief Special handling of the first point in an added line sequence.
     *
     * If the new sequence of lines does not start at the current end point
     * of the path this jump is marked as `PrintFeatureType::NoneType`.
     * \param from The initial point of a polygon.
     */
    void handleInitialPoint(const Point3LL& initial_point)
    {
        if (points.size() == 0)
        {
            addPoint3D(initial_point);
        }
        else if (initial_point != last_point)
        {
            addLineSegment(PrintFeatureType::NoneType, initial_point, 1, 0, 0.0);
        }
    }

    /*!
     * \brief Transfers the currently buffered line segments to the layer
     * message storage.
     */
    void flushPathSegments()
    {
        if (line_types.empty())
        {
            return; // Nothing to do.
        }

        std::shared_ptr<proto::LayerOptimized> proto_layer = _cs_private_data.getOptimizedLayerById(_layer_nr);

        proto::PathSegment* path_segment = proto_layer->add_path_segment();
        path_segment->set_extruder(extruder);
        path_segment->set_point_type(data_point_type);

        std::string line_type_data;
        line_type_data.append(reinterpret_cast<const char*>(line_types.data()), line_types.size() * sizeof(PrintFeatureType));
        line_types.clear();
        path_segment->set_line_type(line_type_data);

        std::string polygon_data;
        polygon_data.append(reinterpret_cast<const char*>(points.data()), points.size() * sizeof(float));
        points.clear();
        path_segment->set_points(polygon_data);

        std::string line_width_data;
        line_width_data.append(reinterpret_cast<const char*>(line_widths.data()), line_widths.size() * sizeof(float));
        line_widths.clear();
        path_segment->set_line_width(line_width_data);

        std::string line_thickness_data;
        line_thickness_data.append(reinterpret_cast<const char*>(line_thicknesses.data()), line_thicknesses.size() * sizeof(float));
        line_thicknesses.clear();
        path_segment->set_line_thickness(line_thickness_data);

        std::string line_velocity_data;
        line_velocity_data.append(reinterpret_cast<const char*>(line_velocities.data()), line_velocities.size() * sizeof(float));
        line_velocities.clear();
        path_segment->set_line_feedrate(line_velocity_data);
    }

    /*!
     * \brief Move the current point of this path to \p position.
     */
    void setCurrentPosition(const Point3LL& position)
    {
        handleInitialPoint(position);
    }

    /*!
     * \brief Adds a single line segment to the current path.
     *
     * The line segment added is from the current last point to point \p to.
     * \param type The type of print feature the line represents (infill, wall,
     * support, etc).
     * \param to The destination coordinate of the line.
     * \param line_width The width of the line.
     * \param line_thickness The thickness (in the Z direction) of the line.
     * \param velocity The velocity of printing this polygon.
     */
    void sendLineTo(const PrintFeatureType& print_feature_type, const Point3LL& to, const coord_t& width, const coord_t& thickness, const Velocity& feedrate)
    {
        assert(! points.empty() && "A point must already be in the buffer for sendLineTo(.) to function properly.");

        if (to != last_point)
        {
            addLineSegment(print_feature_type, to, width, thickness, feedrate);
        }
    }

private:
    /*!
     * \brief Convert and add a point to the points buffer.
     *
     * Each point is represented as three consecutive floats. All members adding a
     * 3D point to the data should use this function.
     */
    void addPoint3D(const Point3LL& point)
    {
        points.push_back(INT2MM(point.x_));
        points.push_back(INT2MM(point.y_));
        points.push_back(INT2MM(point.z_));
        last_point = point;
    }

    /*!
     * \brief Implements the functionality of adding a single 2D line segment to
     * the path data.
     *
     * All member functions adding a 2D line segment should use this functions.
     * \param print_feature_type The type of feature that the polygon is part of
     * (infill, wall, etc).
     * \param point The destination point of the line segment.
     * \param width The width of the lines of the polygon.
     * \param thickness The layer thickness of the polygon.
     * \param velocity How fast the polygon is printed.
     */
    void addLineSegment(const PrintFeatureType& print_feature_type, const Point3LL& point, const coord_t& width, const coord_t& thickness, const Velocity& velocity)
    {
        addPoint3D(point);
        line_types.push_back(print_feature_type);
        line_widths.push_back(INT2MM(width));
        line_thicknesses.push_back(INT2MM(thickness));
        line_velocities.push_back(velocity);
    }
};

ArcusCommunication::ArcusCommunication()
    : private_data(new Private)
    , path_compiler(new PathCompiler(*private_data))
{
}

ArcusCommunication::~ArcusCommunication()
{
    spdlog::info("Closing connection.");
    private_data->socket->close();
    delete private_data->socket;
}

void ArcusCommunication::connect(const std::string& ip, const uint16_t port)
{
    private_data->socket = new Arcus::Socket;
    private_data->socket->addListener(new Listener);

    private_data->socket->registerMessageType(&cura::proto::Slice::default_instance());
    private_data->socket->registerMessageType(&cura::proto::Layer::default_instance());
    private_data->socket->registerMessageType(&cura::proto::LayerOptimized::default_instance());
    private_data->socket->registerMessageType(&cura::proto::Progress::default_instance());
    private_data->socket->registerMessageType(&cura::proto::GCodeLayer::default_instance());
    private_data->socket->registerMessageType(&cura::proto::PrintTimeMaterialEstimates::default_instance());
    private_data->socket->registerMessageType(&cura::proto::SettingList::default_instance());
    private_data->socket->registerMessageType(&cura::proto::GCodePrefix::default_instance());
    private_data->socket->registerMessageType(&cura::proto::SlicingFinished::default_instance());
    private_data->socket->registerMessageType(&cura::proto::SettingExtruder::default_instance());

    spdlog::info("Connecting to {}:{}", ip, port);
    private_data->socket->connect(ip, port);
    auto socket_state = private_data->socket->getState();
    while (socket_state != Arcus::SocketState::Connected && socket_state != Arcus::SocketState::Error)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(private_data->millisecUntilNextTry)); // Wait until we're connected. Check every XXXms.
        socket_state = private_data->socket->getState();
    }
    if (socket_state == Arcus::SocketState::Connected)
    {
        spdlog::info("Connected to {}:{}", ip, port);
    }
}

// On the one hand, don't expose the socket for normal use, but on the other, we need to mock it for unit-tests.
void ArcusCommunication::setSocketMock(Arcus::Socket* socket)
{
    private_data->socket = socket;
}

void ArcusCommunication::beginGCode()
{
    FffProcessor::getInstance()->setTargetStream(&private_data->gcode_output_stream);
}

void ArcusCommunication::flushGCode()
{
    std::string gcode_output_stream = private_data->gcode_output_stream.str();
    auto message_str = slots::instance().modify<plugins::v0::SlotID::POSTPROCESS_MODIFY>(gcode_output_stream);
    if (message_str.size() == 0)
    {
        return;
    }
    std::shared_ptr<proto::GCodeLayer> message = std::make_shared<proto::GCodeLayer>();
    message->set_data(message_str);

    // Send the g-code to the front-end! Yay!
    private_data->socket->sendMessage(message);

    private_data->gcode_output_stream.str("");
}

bool ArcusCommunication::isSequential() const
{
    return false; // We don't necessarily need to send the start g-code before the rest. We can send it afterwards when we have more accurate print statistics.
}

bool ArcusCommunication::hasSlice() const
{
    return private_data->socket->getState() != Arcus::SocketState::Closed && private_data->socket->getState() != Arcus::SocketState::Error
        && private_data->slice_count < 1; // Only slice once per run of CuraEngine. See documentation of slice_count.
}

void ArcusCommunication::sendCurrentPosition(const Point3LL& position)
{
    path_compiler->setCurrentPosition(position);
}

void ArcusCommunication::sendGCodePrefix(const std::string& prefix) const
{
    std::shared_ptr<proto::GCodePrefix> message = std::make_shared<proto::GCodePrefix>();
    std::string message_str = prefix;
    message->set_data(slots::instance().modify<plugins::v0::SlotID::POSTPROCESS_MODIFY>(message_str));
    private_data->socket->sendMessage(message);
}

void ArcusCommunication::sendSliceUUID(const std::string& slice_uuid) const
{
    std::shared_ptr<proto::SliceUUID> message = std::make_shared<proto::SliceUUID>();
    message->set_slice_uuid(slice_uuid);
    private_data->socket->sendMessage(message);
}

void ArcusCommunication::sendFinishedSlicing() const
{
    std::shared_ptr<proto::SlicingFinished> done_message = std::make_shared<proto::SlicingFinished>();
    private_data->socket->sendMessage(done_message);
    spdlog::debug("Sent slicing finished message.");
}

void ArcusCommunication::sendLayerComplete(const LayerIndex::value_type& layer_nr, const coord_t& z, const coord_t& thickness)
{
    std::shared_ptr<proto::LayerOptimized> layer = private_data->getOptimizedLayerById(layer_nr);
    layer->set_height(z);
    layer->set_thickness(thickness);
}

void ArcusCommunication::sendLineTo(const PrintFeatureType& type, const Point3LL& to, const coord_t& line_width, const coord_t& line_thickness, const Velocity& velocity)
{
    path_compiler->sendLineTo(type, to, line_width, line_thickness, velocity);
}

void ArcusCommunication::sendOptimizedLayerData()
{
    path_compiler->flushPathSegments(); // Make sure the last path segment has been flushed from the compiler.

    SliceDataStruct<proto::LayerOptimized>& data = private_data->optimized_layers;
    data.sliced_objects++;
    data.current_layer_offset = data.current_layer_count;
    if (data.sliced_objects < private_data->object_count) // Nothing to send.
    {
        return;
    }
    spdlog::info("Sending {} layers.", data.current_layer_count);

    for (const auto& entry : data.slice_data) // Note: This is in no particular order!
    {
        spdlog::debug("Sending layer data for layer {} of {}.", entry.first, data.slice_data.size());
        private_data->socket->sendMessage(entry.second); // Send the actual layers.
    }
    data.sliced_objects = 0;
    data.current_layer_count = 0;
    data.current_layer_offset = 0;
    data.slice_data.clear();
}

void ArcusCommunication::sendPrintTimeMaterialEstimates() const
{
    spdlog::debug("Sending print time and material estimates.");
    std::shared_ptr<proto::PrintTimeMaterialEstimates> message = std::make_shared<proto::PrintTimeMaterialEstimates>();

    std::vector<Duration> time_estimates = FffProcessor::getInstance()->getTotalPrintTimePerFeature();
    message->set_time_infill(time_estimates[static_cast<unsigned char>(PrintFeatureType::Infill)]);
    message->set_time_inset_0(time_estimates[static_cast<unsigned char>(PrintFeatureType::OuterWall)]);
    message->set_time_inset_x(time_estimates[static_cast<unsigned char>(PrintFeatureType::InnerWall)]);
    message->set_time_none(time_estimates[static_cast<unsigned char>(PrintFeatureType::NoneType)]);
    message->set_time_retract(time_estimates[static_cast<unsigned char>(PrintFeatureType::StationaryRetractUnretract)]);
    message->set_time_skin(time_estimates[static_cast<unsigned char>(PrintFeatureType::Skin)]);
    message->set_time_skirt(time_estimates[static_cast<unsigned char>(PrintFeatureType::SkirtBrim)]);
    message->set_time_support(time_estimates[static_cast<unsigned char>(PrintFeatureType::Support)]);
    message->set_time_support_infill(time_estimates[static_cast<unsigned char>(PrintFeatureType::SupportInfill)]);
    message->set_time_support_interface(time_estimates[static_cast<unsigned char>(PrintFeatureType::SupportInterface)]);
    message->set_time_prime_tower(time_estimates[static_cast<unsigned char>(PrintFeatureType::PrimeTower)]);
    message->set_time_travel(
        time_estimates[static_cast<unsigned char>(PrintFeatureType::MoveUnretracted)] + time_estimates[static_cast<unsigned char>(PrintFeatureType::MoveRetracted)]
        + time_estimates[static_cast<unsigned char>(PrintFeatureType::MoveWhileRetracting)] + time_estimates[static_cast<unsigned char>(PrintFeatureType::MoveWhileUnretracting)]);

    for (size_t extruder_nr = 0; extruder_nr < Application::getInstance().current_slice_->scene.extruders.size(); extruder_nr++)
    {
        proto::MaterialEstimates* material_message = message->add_materialestimates();
        material_message->set_id(extruder_nr);
        material_message->set_material_amount(FffProcessor::getInstance()->getTotalFilamentUsed(extruder_nr));
    }

    private_data->socket->sendMessage(message);
    spdlog::debug("Done sending print time and material estimates.");
}

void ArcusCommunication::sendProgress(double progress) const
{
    const int rounded_amount = 1000 * progress;
    if (private_data->last_sent_progress == rounded_amount) // No need to send another tiny update step.
    {
        return;
    }

    std::shared_ptr<proto::Progress> message = std::make_shared<cura::proto::Progress>();
    double progress_all_objects = progress / private_data->object_count;
    progress_all_objects += private_data->optimized_layers.sliced_objects * (1.0 / private_data->object_count);
    message->set_amount(progress_all_objects);
    private_data->socket->sendMessage(message);

    private_data->last_sent_progress = rounded_amount;
}

void ArcusCommunication::setLayerForSend(const LayerIndex::value_type& layer_nr)
{
    path_compiler->setLayer(layer_nr);
}

void ArcusCommunication::setExtruderForSend(const ExtruderTrain& extruder)
{
    path_compiler->setExtruder(extruder);
}

void ArcusCommunication::sliceNext()
{
    const Arcus::MessagePtr message = private_data->socket->takeNextMessage();

    // Handle the main Slice message.
    const cura::proto::Slice* slice_message = dynamic_cast<cura::proto::Slice*>(message.get()); // See if the message is of the message type Slice. Returns nullptr otherwise.
    if (slice_message == nullptr)
    {
        return;
    }
    spdlog::debug("Received a Slice message.");

#ifdef SENTRY_URL
    sentry_value_t user = sentry_value_new_object();
    sentry_value_set_by_key(user, "id", sentry_value_new_string(slice_message->sentry_id().c_str()));
    if (slice_message->has_user_name())
    {
        spdlog::debug("Setting Sentry user to {}", slice_message->user_name());
        sentry_value_set_by_key(user, "username", sentry_value_new_string(slice_message->user_name().c_str()));
    }
    sentry_set_user(user);
    sentry_set_tag("cura.version", slice_message->cura_version().c_str());
    if (slice_message->has_project_name())
    {
        sentry_set_tag("cura.project_name", slice_message->project_name().c_str());
    }
#endif

#ifdef ENABLE_PLUGINS
    for (const auto& plugin : slice_message->engine_plugins())
    {
        const auto slot_id = static_cast<plugins::v0::SlotID>(plugin.id());
        slots::instance().connect(slot_id, plugin.plugin_name(), plugin.plugin_version(), utils::createChannel({ plugin.address(), plugin.port() }));
#ifdef SENTRY_URL
        sentry_set_tag(fmt::format("plugin_{}.version", plugin.plugin_name()).c_str(), plugin.plugin_version().c_str());
#endif
    }
#endif // ENABLE_PLUGINS

    auto slice = std::make_shared<Slice>(slice_message->object_lists().size());
    Application::getInstance().current_slice_ = slice;

    private_data->readGlobalSettingsMessage(slice_message->global_settings());
    private_data->readExtruderSettingsMessage(slice_message->extruders());

    // Broadcast the settings to the plugins
    slots::instance().broadcast<plugins::v0::SlotID::SETTINGS_BROADCAST>(*slice_message);
    const size_t extruder_count = slice->scene.extruders.size();

    // For each setting, register what extruder it should be obtained from (if this is limited to an extruder).
    for (const cura::proto::SettingExtruder& setting_extruder : slice_message->limit_to_extruder())
    {
        const int32_t extruder_nr = setting_extruder.extruder(); // Cast from proto::int to int32_t!
        if (extruder_nr < 0 || extruder_nr > static_cast<int32_t>(extruder_count))
        {
            // If it's -1 it should be ignored as per the spec. Let's also ignore it if it's beyond range.
            continue;
        }
        ExtruderTrain& extruder = slice->scene.extruders[setting_extruder.extruder()];
        slice->scene.limit_to_extruder.emplace(setting_extruder.name(), &extruder);
    }

    // Load all mesh groups, meshes and their settings.
    private_data->object_count = 0;
    for (const cura::proto::ObjectList& mesh_group_message : slice_message->object_lists())
    {
        private_data->readMeshGroupMessage(mesh_group_message);
    }
    spdlog::debug("Done reading Slice message.");

    if (! slice->scene.mesh_groups.empty())
    {
        slice->compute();
        FffProcessor::getInstance()->finalize();
        flushGCode();
        sendPrintTimeMaterialEstimates();
        sendFinishedSlicing();
        slice.reset();
        private_data->slice_count++;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250)); // Pause before checking again for a slice message.
}

} // namespace cura

#endif // ARCUS
