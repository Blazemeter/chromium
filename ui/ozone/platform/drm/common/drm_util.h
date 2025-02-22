// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_DRM_COMMON_DRM_UTIL_H_
#define UI_OZONE_PLATFORM_DRM_COMMON_DRM_UTIL_H_

#include <stddef.h>

#include <memory>
#include <vector>

#include "base/files/file_path.h"
#include "base/macros.h"
#include "ui/display/types/display_snapshot.h"
#include "ui/ozone/common/gpu/ozone_gpu_message_params.h"
#include "ui/ozone/platform/drm/common/display_types.h"
#include "ui/ozone/platform/drm/common/scoped_drm_types.h"

typedef struct _drmModeModeInfo drmModeModeInfo;

namespace display {
class DisplayMode;
class EdidParser;
}  // namespace display

namespace gfx {
class Point;
}

namespace ui {

class HardwareDisplayControllerInfo;
using HardwareDisplayControllerInfos =
    std::vector<std::unique_ptr<HardwareDisplayControllerInfo>>;

// These values are persisted to logs. Entries should not be renumbered and
// numeric values should never be reused.
enum class EdidColorSpaceChecksOutcome {
  kSuccess = 0,
  kErrorBadCoordinates = 1,
  kErrorPrimariesAreaTooSmall = 2,
  kErrorBluePrimaryIsBroken = 3,
  kErrorCannotExtractToXYZD50 = 4,
  kErrorBadGamma = 5,
  kMaxValue = kErrorBadGamma
};

// Representation of the information required to initialize and configure a
// native display. |index| is the position of the connection and will be
// used to generate a unique identifier for the display.
class HardwareDisplayControllerInfo {
 public:
  HardwareDisplayControllerInfo(ScopedDrmConnectorPtr connector,
                                ScopedDrmCrtcPtr crtc,
                                size_t index);
  ~HardwareDisplayControllerInfo();

  drmModeConnector* connector() const { return connector_.get(); }
  drmModeCrtc* crtc() const { return crtc_.get(); }
  ScopedDrmCrtcPtr release_crtc() { return std::move(crtc_); }
  void set_crtc(ScopedDrmCrtcPtr crtc) { crtc_ = std::move(crtc); }
  size_t index() const { return index_; }

  bool has_associated_crtc() const { return crtc_.get(); }

 private:
  ScopedDrmConnectorPtr connector_;
  ScopedDrmCrtcPtr crtc_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HardwareDisplayControllerInfo);
};

// Looks-up and parses the native display configurations returning all available
// displays. The boolean value indicates whether device has enough hardware
// resource to support all of displays.
HardwareDisplayControllerInfos GetAvailableDisplayControllerInfos(
    int fd,
    bool* support_all_connectors);

bool SameMode(const drmModeModeInfo& lhs, const drmModeModeInfo& rhs);

std::unique_ptr<display::DisplayMode> CreateDisplayMode(
    const drmModeModeInfo& mode);

// Extracts the display modes list from |info| as well as the current and native
// display modes given the |active_pixel_size| which is retrieved from the first
// detailed timing descriptor in the EDID.
display::DisplaySnapshot::DisplayModeList ExtractDisplayModes(
    HardwareDisplayControllerInfo* info,
    const gfx::Size& active_pixel_size,
    const display::DisplayMode** out_current_mode,
    const display::DisplayMode** out_native_mode);

// |info| provides the DRM information related to the display, |fd| is the
// connection to the DRM device.
std::unique_ptr<display::DisplaySnapshot> CreateDisplaySnapshot(
    HardwareDisplayControllerInfo* info,
    int fd,
    const base::FilePath& sys_path,
    size_t device_index,
    const gfx::Point& origin);

std::unique_ptr<display::DisplaySnapshot> CreateDisplaySnapshot(
    const DisplaySnapshot_Params& params);

// Creates a serialized version of MovableDisplaySnapshots for IPC transmission.
std::vector<DisplaySnapshot_Params> CreateDisplaySnapshotParams(
    const MovableDisplaySnapshots& displays);

int GetFourCCFormatForOpaqueFramebuffer(gfx::BufferFormat format);

gfx::Size GetMaximumCursorSize(int fd);

ScopedDrmPropertyPtr FindDrmProperty(int fd,
                                     drmModeObjectProperties* properties,
                                     const char* name);

bool HasColorCorrectionMatrix(int fd, drmModeCrtc* crtc);

DisplayMode_Params GetDisplayModeParams(const display::DisplayMode& mode);

std::unique_ptr<display::DisplayMode> CreateDisplayModeFromParams(
    const DisplayMode_Params& pmode);

bool MatchMode(const display::DisplayMode& display_mode,
               const drmModeModeInfo& m);

const gfx::Size ModeSize(const drmModeModeInfo& mode);

float ModeRefreshRate(const drmModeModeInfo& mode);

bool ModeIsInterlaced(const drmModeModeInfo& mode);

OverlaySurfaceCandidateList CreateOverlaySurfaceCandidateListFrom(
    const std::vector<OverlayCheck_Params>& params);

std::vector<OverlayCheck_Params> CreateParamsFromOverlaySurfaceCandidate(
    const OverlaySurfaceCandidateList& candidates);

OverlayStatusList CreateOverlayStatusListFrom(
    const std::vector<OverlayCheckReturn_Params>& params);

std::vector<OverlayCheckReturn_Params> CreateParamsFromOverlayStatusList(
    const OverlayStatusList& returns);

// Uses |edid_parser| to extract a gfx::ColorSpace which will be IsValid() if
// both gamma and the color primaries were correctly found.
gfx::ColorSpace GetColorSpaceFromEdid(const display::EdidParser& edid_parser);

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_DRM_COMMON_DRM_UTIL_H_
