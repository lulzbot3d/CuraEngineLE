//Copyright (c) 2019 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#include "GCodeExportTest.h"

namespace cura
{
CPPUNIT_TEST_SUITE_REGISTRATION(GCodeExportTest);

void GCodeExportTest::setUp()
{
    output.clear();
    gcode.output_stream = &output;

    //Since GCodeExport doesn't support copying, we have to reset everything in-place.
    gcode.currentPosition = Point3(0, 0, MM2INT(20));
    gcode.layer_nr = 0;
    gcode.current_e_value = 0;
    gcode.current_extruder = 0;
    gcode.current_fan_speed = -1;
    gcode.total_print_times = std::vector<Duration>(static_cast<unsigned char>(PrintFeatureType::NumPrintFeatureTypes), 0.0);
    gcode.currentSpeed = 1;
    gcode.current_print_acceleration = -1;
    gcode.current_travel_acceleration = -1;
    gcode.current_jerk = -1;
    gcode.current_max_z_feedrate = -1;
    gcode.is_z_hopped = 0;
    gcode.setFlavor(EGCodeFlavor::MARLIN);
    gcode.initial_bed_temp = 0;
    gcode.extruder_count = 0;
    gcode.fan_number = 0;
    gcode.total_bounding_box = AABB3D();
}

void GCodeExportTest::commentEmpty()
{
    gcode.writeComment("");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("Semicolon must exist but it must be empty for the rest.",
        std::string(";"), output.str());
}

} //namespace cura