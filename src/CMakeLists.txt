#===============================================================================
#
# Copyright (C) 2019 Istituto Italiano di Tecnologia (IIT)
#
# This software may be modified and distributed under the terms of the
# GPL-2+ license. See the accompanying LICENSE file for details.
#
#===============================================================================

add_subdirectory(gaze-ctrl-library)
add_subdirectory(icub-fingers-encoders)
add_subdirectory(sfm-library)
add_subdirectory(object-tracking-depth)
add_subdirectory(object-tracking)

option(BUILD_OBJECT_TRACKING_LOGGER "Build object-tracking-logger" OFF)
if (BUILD_OBJECT_TRACKING_LOGGER)
  add_subdirectory(object-tracking-logger)
endif()

option(BUILD_OBJECT_TRACKING_GROUND_TRUTH "Build object-tracking-ground-truth" OFF)
if (BUILD_OBJECT_TRACKING_GROUND_TRUTH)
  add_subdirectory(object-tracking-ground-truth)
endif()

option(BUILD_OBJECT_TRACKING_MANIPULATION "Build object-tracking-manipulation" OFF)
if (BUILD_OBJECT_TRACKING_MANIPULATION)
  add_subdirectory(object-tracking-manipulation)
endif()

option(BUILD_OBJECT_TRACKING_PLAYBACK "Build object-tracking-playback" OFF)
if (BUILD_OBJECT_TRACKING_PLAYBACK)
  add_subdirectory(object-tracking-playback)
endif()

option(BUILD_OBJECT_TRACKING_VIEWER "Build object-tracking-viewer" OFF)
if (BUILD_OBJECT_TRACKING_VIEWER)
  add_subdirectory(object-tracking-viewer)
endif()
