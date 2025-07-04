// Copyright (c) 2021 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef PATHORDERMONOTONIC_H
#define PATHORDERMONOTONIC_H

#include <cmath> //For std::sin() and std::cos().
#include <unordered_map> //To track monotonic sequences.
#include <unordered_set> //To track starting points of monotonic sequences.

#include "PathOrder.h"
#include "path_ordering.h"

namespace cura
{

/*!
 * Class that orders paths monotonically.
 *
 * This is a utility class that changes the order in which things are printed,
 * to ensure that they are printed in the same major direction. Printing
 * adjacent lines in the same direction ensures that they layer on top of each
 * other in the same way. That helps to make the entire surface look consistent.
 *
 * To use this class, first create an instance and provide some parameters as
 * metadata. Then add polygons and polylines to the class. Then call the
 * \ref optimize function to compute the order. Finally, print the polygons and
 * polylines in the \ref paths field in the order in which they are given.
 *
 * In the output of this class, polylines and polygons are combined into a
 * single list: \ref paths . Each path contains a pointer to the original
 * polygon data, as well as whether that data represented a polygon or a
 * polyline, which direction to print the path in, and where to start along the
 * path.
 *
 * The monotonic order does not use the Z seam settings. It is meant to apply
 * only to polylines. If given polygons, it will place the seam in the location
 * closest to the source direction of the monotonicity vector.
 */
template<typename PathType>
class PathOrderMonotonic : public PathOrder<PathType>
{
public:
    using Path = PathOrdering<PathType>;
    using PathOrder<PathType>::coincident_point_distance_;

    PathOrderMonotonic(const AngleRadians monotonic_direction, const coord_t max_adjacent_distance, const Point2LL start_point)
        : PathOrder<PathType>(start_point)
        // The monotonic vector needs to rotate clockwise instead of counter-clockwise, the same as how the infill patterns are generated.
        , monotonic_vector_(-std::cos(monotonic_direction) * monotonic_vector_resolution_, std::sin(monotonic_direction) * monotonic_vector_resolution_)
        , max_adjacent_distance_(max_adjacent_distance)
    {
    }

    void optimize()
    {
        if (this->paths_.empty())
        {
            return;
        }

        // Get the vertex data and store it in the paths.
        for (Path& path : this->paths_)
        {
            path.converted_ = &path.getVertexData();
        }

        std::vector<Path> reordered; // To store the result in. At the end, we'll std::swap with the real paths.
        reordered.reserve(this->paths_.size());

        // First print all the looping polygons, if there are any.
        std::vector<Path*> polylines; // Also find all polylines and store them in a vector that we can sort in-place without making copies all the time.
        this->detectLoops(); // Always filter out loops. We don't specifically want to print those in monotonic order.
        for (Path& path : this->paths_)
        {
            if (path.is_closed_ || path.vertices_->size() <= 1)
            {
                reordered.push_back(path);
            }
            else
            {
                polylines.push_back(&path);
                // Assign an invalid starting vertex to indicate we don't know the starting point yet.
                polylines.back()->start_vertex_ = polylines.back()->converted_->size();
            }
        }

        // Sort the polylines by their projection on the monotonic vector. This helps find adjacent lines quickly.
        std::stable_sort(
            polylines.begin(),
            polylines.end(),
            [this](Path* a, Path* b)
            {
                const coord_t a_start_projection = dot(a->converted_->front(), monotonic_vector_);
                const coord_t a_end_projection = dot(a->converted_->back(), monotonic_vector_);
                const coord_t a_projection = std::min(a_start_projection, a_end_projection); // The projection of a path is the endpoint furthest back of the two endpoints.

                const coord_t b_start_projection = dot(b->converted_->front(), monotonic_vector_);
                const coord_t b_end_projection = dot(b->converted_->back(), monotonic_vector_);
                const coord_t b_projection = std::min(b_start_projection, b_end_projection);

                return a_projection < b_projection;
            });
        // Create a bucket grid to be able to find adjacent lines quickly.
        SparsePointGridInclusive<Path*> line_bucket_grid(MM2INT(2)); // Grid size of 2mm.
        for (Path* polyline : polylines)
        {
            if (! polyline->converted_->empty())
            {
                line_bucket_grid.insert(polyline->converted_->front(), polyline);
                line_bucket_grid.insert(polyline->converted_->back(), polyline);
            }
        }

        // Create sequences of line segments that get printed together in a monotonic direction.
        // There are several constraints we impose here:
        //  - Strings of incident polylines are printed in sequence. That is, if their endpoints are incident.
        //    - The endpoint of the string that is earlier in the monotonic direction will get printed first.
        //    - The start_vertex of this line will already be set to indicate where to start from.
        //  - If a line overlaps with another line in the perpendicular direction, and is within max_adjacent_distance (~1 line width) in the monotonic direction, it must be
        //  printed in monotonic order.
        //    - The earlier line is marked as being in sequence with the later line.
        //    - The later line is no longer a starting point, unless there are multiple adjacent lines before it.
        // The ``starting_lines`` set indicates possible locations to start from. Each starting line represents one "sequence", which is either a set of adjacent line segments or a
        // string of polylines. The ``connections`` map indicates, starting from each starting segment, the sequence of line segments to print in order. Note that for performance
        // reasons, the ``connections`` map will sometimes link the end of one segment to the start of the next segment. This link should be ignored.
        const Point2LL perpendicular = turn90CCW(monotonic_vector_); // To project on to detect adjacent lines.

        std::unordered_set<Path*> connected_lines; // Lines that are reachable from one of the starting lines through its connections.
        std::unordered_set<Path*> starting_lines; // Starting points of a linearly connected segment.
        std::unordered_map<Path*, Path*> connections; // For each polyline, which polyline it overlaps with, closest in the projected order.

        for (auto polyline_it = polylines.begin(); polyline_it != polylines.end(); polyline_it++)
        {
            if (connections.contains(*polyline_it)) // Already connected this one through a polyline.
            {
                continue;
            }
            // First find out if this polyline is part of a string of polylines.
            std::deque<Path*> polystring = findPolylineString(*polyline_it, line_bucket_grid, monotonic_vector_);

            // If we're part of a string of polylines, connect up the whole string and mark all of them as being connected.
            if (polystring.size() > 1)
            {
                starting_lines.insert(polystring[0]);
                for (size_t i = 0; i < polystring.size() - 1; ++i) // Iterate over every pair of adjacent polylines in the string (so skip the last one)!
                {
                    connections[polystring[i]] = polystring[i + 1];
                    connected_lines.insert(polystring[i + 1]);

                    // Even though we chain polylines, we still want to find lines that they overlap with.
                    // The strings of polylines may still have weird shapes which interweave with other strings of polylines or loose lines.
                    // So when a polyline string comes into contact with other lines, we still want to guarantee their order.
                    // So here we will look for which lines they come into contact with, and thus mark those as possible starting points, so that they function as a new junction.
                    const std::vector<Path*> overlapping_lines = getOverlappingLines(std::find(polylines.begin(), polylines.end(), polystring[i]), perpendicular, polylines);
                    for (Path* overlapping_line : overlapping_lines)
                    {
                        if (std::find(polystring.begin(), polystring.end(), overlapping_line)
                            == polystring.end()) // Mark all overlapping lines not part of the string as possible starting points.
                        {
                            starting_lines.insert(overlapping_line);
                            starting_lines.insert(polystring[i + 1]); // Also be able to re-start from this point in the string.
                        }
                    }
                }
            }
            else // Not a string of polylines, but simply adjacent line segments.
            {
                if (! connected_lines.contains(*polyline_it)) // Nothing connects to this line yet.
                {
                    starting_lines.insert(*polyline_it); // This is a starting point then.
                }
                const std::vector<Path*> overlapping_lines = getOverlappingLines(polyline_it, perpendicular, polylines);
                if (overlapping_lines.size() == 1) // If we're not a string of polylines, but adjacent to only one other polyline, create a sequence of polylines.
                {
                    connections[*polyline_it] = overlapping_lines[0];
                    if (connected_lines.contains(overlapping_lines[0])) // This line was already connected to.
                    {
                        starting_lines.insert(overlapping_lines[0]); // Multiple lines connect to it, so we must be able to start there.
                    }
                    else
                    {
                        connected_lines.insert(overlapping_lines[0]);
                    }
                }
                else // Either 0 (the for loop terminates immediately) or multiple overlapping lines. For multiple lines we need to mark all of them a starting position.
                {
                    for (Path* overlapping_line : overlapping_lines)
                    {
                        starting_lines.insert(overlapping_line);
                    }
                }
            }
        }

        // Order the starting points of each segments monotonically. This is the order in which to print each segment.
        std::vector<Path*> starting_lines_monotonic;
        starting_lines_monotonic.resize(starting_lines.size());
        std::partial_sort_copy(
            starting_lines.begin(),
            starting_lines.end(),
            starting_lines_monotonic.begin(),
            starting_lines_monotonic.end(),
            [this](Path* a, Path* b)
            {
                const coord_t a_start_projection = dot(a->converted_->front(), monotonic_vector_);
                const coord_t a_end_projection = dot(a->converted_->back(), monotonic_vector_);
                const coord_t a_projection_min = std::min(a_start_projection, a_end_projection); // The projection of a path is the endpoint furthest back of the two endpoints.
                const coord_t a_projection_max = std::max(
                    a_start_projection,
                    a_end_projection); // But in case of ties, the other endpoint counts too. Important for polylines where multiple endpoints have the same position!

                const coord_t b_start_projection = dot(b->converted_->front(), monotonic_vector_);
                const coord_t b_end_projection = dot(b->converted_->back(), monotonic_vector_);
                const coord_t b_projection_min = std::min(b_start_projection, b_end_projection);
                const coord_t b_projection_max = std::max(b_start_projection, b_end_projection);

                return a_projection_min < b_projection_min || (a_projection_min == b_projection_min && a_projection_max < b_projection_max);
            });

        // Now that we have the segments of overlapping lines, and know in which order to print the segments, print segments in monotonic order.
        Point2LL current_pos = this->start_point_;
        for (Path* line : starting_lines_monotonic)
        {
            optimizeClosestStartPoint(*line, current_pos);
            reordered.push_back(*line); // Plan the start of the sequence to be printed next!
            auto connection = connections.find(line);

            std::unordered_map<Path*, Path*> checked_connections; // Which connections have already been iterated over
            auto checked_connection = checked_connections.find(line);

            while (connection != connections.end() // Stop if the sequence ends
                   && starting_lines.find(connection->second) == starting_lines.end() // or if we hit another starting point.
                   && (checked_connection == checked_connections.end()
                       || checked_connection->second != connection->second)) // or if we have already checked the connection (to avoid falling into a cyclical connection)
            {
                checked_connections.insert({ connection->first, connection->second });
                line = connection->second;
                optimizeClosestStartPoint(*line, current_pos);
                reordered.push_back(*line); // Plan this line in, to be printed next!
                connection = connections.find(line);
                checked_connection = checked_connections.find(line);
            }
        }

        std::swap(reordered, this->paths_); // Store the resulting list in the main paths.
    }

protected:
    /*!
     * The direction in which to print monotonically, encoded as vector of length
     * ``monotonic_vector_resolution``.
     *
     * The resulting ordering will cause clusters of paths to be sorted
     * according to their projection on this vector.
     */
    Point2LL monotonic_vector_;

    /*!
     * Maximum distance at which lines are considered to be adjacent.
     *
     * The monotonicity constraint is only held for lines that are closer than
     * this distance together.
     */
    coord_t max_adjacent_distance_;

    /*!
     * For a given path, make sure that it is configured correctly to start
     * printing from the best endpoint.
     *
     * This changes the path's ``start_vertex`` and ``backwards`` fields, and
     * also adjusts the \ref current_pos in-place.
     *
     * If the path already had a ``start_vertex`` set, this will not be
     * adjusted. Only the ``current_pos`` will be set then.
     *
     * Will cause a crash if given a path with 0 vertices!
     * \param path The path to adjust the start and direction parameters for.
     * \param current_pos The last position of the nozzle before printing this
     * path.
     */
    void optimizeClosestStartPoint(Path& path, Point2LL& current_pos)
    {
        if (path.start_vertex_ == path.converted_->size())
        {
            const coord_t dist_start = vSize2(current_pos - path.converted_->front());
            const coord_t dist_end = vSize2(current_pos - path.converted_->back());
            if (dist_start < dist_end)
            {
                path.start_vertex_ = 0;
                path.backwards_ = false;
            }
            else
            {
                path.start_vertex_ = path.converted_->size() - 1;
                path.backwards_ = true;
            }
        }
        current_pos = (*path.converted_)[path.converted_->size() - 1 - path.start_vertex_]; // Opposite of the start vertex.
    }

    /*!
     * Some input contains line segments or polylines that are separate paths,
     * but are still intended to be printed as a long sequence. This function
     * finds such strings of polylines.
     * \param polyline Any polyline which may be part of a string of polylines.
     * \param line_bucket_grid A pre-computed bucket grid to allow quick look-up
     * of which vertices are nearby.
     * \return A list of polylines, in the order in which they should be
     * printed. All paths in this string already have their start_vertex set
     * correctly.
     */
    std::deque<Path*> findPolylineString(Path* polyline, const SparsePointGridInclusive<Path*>& line_bucket_grid, const Point2LL monotonic_vector)
    {
        std::deque<Path*> result;
        if (polyline->converted_->empty())
        {
            return result;
        }

        // Find the two endpoints of the polyline string, on either side.
        result.push_back(polyline);
        polyline->start_vertex_ = 0;
        Point2LL first_endpoint = polyline->converted_->front();
        Point2LL last_endpoint = polyline->converted_->back();
        std::vector<SparsePointGridInclusiveImpl::SparsePointGridInclusiveElem<Path*>> lines_before = line_bucket_grid.getNearby(first_endpoint, coincident_point_distance_);
        auto close_line_before = std::find_if(
            lines_before.begin(),
            lines_before.end(),
            [first_endpoint](SparsePointGridInclusiveImpl::SparsePointGridInclusiveElem<Path*> found_path)
            {
                return canConnectToPolyline(first_endpoint, found_path);
            });
        std::vector<SparsePointGridInclusiveImpl::SparsePointGridInclusiveElem<Path*>> lines_after = line_bucket_grid.getNearby(last_endpoint, coincident_point_distance_);
        auto close_line_after = std::find_if(
            lines_after.begin(),
            lines_after.end(),
            [last_endpoint](SparsePointGridInclusiveImpl::SparsePointGridInclusiveElem<Path*> found_path)
            {
                return canConnectToPolyline(last_endpoint, found_path);
            });

        while (close_line_before != lines_before.end())
        {
            Path* first = close_line_before->val;
            result.push_front(first); // Store this one in the sequence. It's a good one.
            size_t farthest_vertex = getFarthestEndpoint(first, close_line_before->point); // Get to the opposite side.
            first->start_vertex_ = farthest_vertex;
            first->backwards_ = farthest_vertex != 0;
            first_endpoint = (*first->converted_)[farthest_vertex];
            lines_before = line_bucket_grid.getNearby(first_endpoint, coincident_point_distance_);
            close_line_before = std::find_if(
                lines_before.begin(),
                lines_before.end(),
                [first_endpoint](SparsePointGridInclusiveImpl::SparsePointGridInclusiveElem<Path*> found_path)
                {
                    return canConnectToPolyline(first_endpoint, found_path);
                });
        }
        while (close_line_after != lines_after.end())
        {
            Path* last = close_line_after->val;
            result.push_back(last);
            size_t farthest_vertex = getFarthestEndpoint(last, close_line_after->point); // Get to the opposite side.
            last->start_vertex_ = (farthest_vertex == 0) ? last->converted_->size() - 1 : 0;
            last->backwards_ = farthest_vertex != 0;
            last_endpoint = (*last->converted_)[farthest_vertex];
            lines_after = line_bucket_grid.getNearby(last_endpoint, coincident_point_distance_);
            close_line_after = std::find_if(
                lines_after.begin(),
                lines_after.end(),
                [last_endpoint](SparsePointGridInclusiveImpl::SparsePointGridInclusiveElem<Path*> found_path)
                {
                    return canConnectToPolyline(last_endpoint, found_path);
                });
        }

        // Figure out which of the two endpoints to start with: The one monotonically earliest.
        const coord_t first_projection = dot(first_endpoint, monotonic_vector);
        const coord_t last_projection = dot(last_endpoint, monotonic_vector);
        // If the last endpoint should be printed first (unlikely due to monotonic start, but possible), flip the whole polyline!
        if (last_projection < first_projection)
        {
            std::reverse(result.begin(), result.end());
            for (Path* path : result) // Also reverse their start_vertex.
            {
                path->start_vertex_ = (path->start_vertex_ == 0) ? path->converted_->size() - 1 : 0;
                path->backwards_ = ! path->backwards_;
            }
        }

        if (result.size() == 1)
        {
            result[0]->start_vertex_ = result[0]->converted_->size(); // Reset start vertex as "unknown" again if it's not a string of polylines.
        }
        return result;
    }

    /*!
     * Get the endpoint of the polyline that is farthest away from the given
     * point.
     * \param polyline The polyline to get an endpoint of.
     * \param point The point to get far away from.
     * \return The vertex index of the endpoint that is farthest away.
     */
    size_t getFarthestEndpoint(Path* polyline, const Point2LL point)
    {
        const coord_t front_dist = vSize2(polyline->converted_->front() - point);
        const coord_t back_dist = vSize2(polyline->converted_->back() - point);
        if (front_dist < back_dist)
        {
            return polyline->converted_->size() - 1;
        }
        else
        {
            return 0;
        }
    }

    /*!
     * Find which lines are overlapping with a certain line.
     * \param polyline_it The line with which to find overlaps. Given as an
     * iterator into the sorted polylines list, to cut down on the search space.
     * If the lines don't have too much overlap, this should result in only a
     * handful of lines being searched at all.
     * \param perpendicular A vector perpendicular to the monotonic vector, pre-
     * calculated.
     * \param polylines The sorted list of polylines.
     */
    std::vector<Path*> getOverlappingLines(const typename std::vector<Path*>::iterator polyline_it, const Point2LL perpendicular, const std::vector<Path*>& polylines)
    {
        const coord_t max_adjacent_projected_distance = max_adjacent_distance_ * monotonic_vector_resolution_;
        // How far this extends in the monotonic direction, to make sure we only go up to max_adjacent_distance in that direction.
        const coord_t start_monotonic = dot((*polyline_it)->converted_->front(), monotonic_vector_);
        const coord_t end_monotonic = dot((*polyline_it)->converted_->back(), monotonic_vector_);
        const coord_t my_farthest_monotonic = std::max(start_monotonic, end_monotonic);
        const coord_t my_closest_monotonic = std::min(start_monotonic, end_monotonic);
        const coord_t my_farthest_monotonic_padded = my_farthest_monotonic + max_adjacent_projected_distance;
        const coord_t my_closest_monotonic_padded = my_closest_monotonic - max_adjacent_projected_distance;
        // How far this line reaches in the perpendicular direction -- the range at which the line overlaps other lines.
        const coord_t my_start = dot((*polyline_it)->converted_->front(), perpendicular);
        const coord_t my_end = dot((*polyline_it)->converted_->back(), perpendicular);
        const coord_t my_farthest = std::max(my_start, my_end);
        const coord_t my_closest = std::min(my_start, my_end);
        const coord_t my_farthest_padded = my_farthest + max_adjacent_projected_distance;
        const coord_t my_closest_padded = my_closest - max_adjacent_projected_distance;

        std::vector<Path*> overlapping_lines;
        for (auto overlapping_line = polyline_it + 1; overlapping_line != polylines.end(); overlapping_line++)
        {
            // Don't go beyond the maximum adjacent distance.
            const coord_t start_their_projection = dot((*overlapping_line)->converted_->front(), monotonic_vector_);
            const coord_t end_their_projection = dot((*overlapping_line)->converted_->back(), monotonic_vector_);
            const coord_t their_farthest_projection = std::max(start_their_projection, end_their_projection);
            const coord_t their_closest_projection = std::min(start_their_projection, end_their_projection);
            // Multiply by the length of the vector since we need to compare actual distances here.
            if (their_closest_projection > my_farthest_monotonic_padded || my_closest_monotonic_padded > their_farthest_projection)
            {
                break; // Too far. This line and all subsequent lines are not adjacent anymore, even though they might be side-by-side.
            }

            // Does this one overlap?
            const coord_t their_start = dot((*overlapping_line)->converted_->front(), perpendicular);
            const coord_t their_end = dot((*overlapping_line)->converted_->back(), perpendicular);
            const coord_t their_farthest = std::max(their_start, their_end);
            const coord_t their_closest = std::min(their_start, their_end);
            /*There are 5 possible cases of overlapping:
            - We are behind them, partially overlapping. my_start is between their_start and their_end.
            - We are in front of them, partially overlapping. my_end is between their_start and their_end.
            - We are a smaller line, they completely overlap us. Both my_start and my_end are between their_start and their_end. (Caught with the first 2 conditions already.)
            - We are a bigger line, and completely overlap them. Both their_start and their_end are between my_start and my_end.
            - Lines are exactly equal. Start and end are the same. (Caught with the previous condition too.)*/
            if ((my_closest_padded >= their_closest && my_closest_padded <= their_farthest) || (my_farthest_padded >= their_closest && my_farthest_padded <= their_farthest)
                || (their_closest >= my_closest_padded && their_farthest <= my_farthest_padded))
            {
                overlapping_lines.push_back(*overlapping_line);
            }
        }

        return overlapping_lines;
    }

protected:
    /*!
     * Length of the monotonic vector, as stored.
     *
     * This needs to be long enough to eliminate rounding errors caused by
     * rounding the coordinates of the vector to integer coordinates for the
     * ``coord_t`` data type, but not so long as to cause integer overflows if
     * the quadratic is multiplied by a projection length.
     */
    constexpr static coord_t monotonic_vector_resolution_ = 1000;

private:
    /*!
     * Predicate to check if a nearby path is okay for polylines to connect
     * with.
     *
     * It is okay if the endpoints are sufficiently close together, and the
     * polyline is not yet connected to a different string of polylines.
     * \param nearby_endpoint The endpoint of the current string of polylines.
     * We'll check if the candidate polyline is nearby enough.
     * \param found_path A candidate polyline, as found in the bucket grid. This
     * struct of the bucket grid contains not only the actual path (via pointer)
     * but also the endpoint of it that it found to be nearby.
     */
    static bool canConnectToPolyline(const Point2LL nearby_endpoint, SparsePointGridInclusiveImpl::SparsePointGridInclusiveElem<Path*> found_path)
    {
        return found_path.val->start_vertex_ == found_path.val->converted_->size() // Don't find any line already in the string.
            && vSize2(found_path.point - nearby_endpoint) < coincident_point_distance_ * coincident_point_distance_; // And only find close lines.
    }
};

} // namespace cura

#endif // PATHORDERMONOTONIC_H
