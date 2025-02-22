// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_WS_WINDOW_TREE_TEST_HELPER_H_
#define SERVICES_WS_WINDOW_TREE_TEST_HELPER_H_

#include <string>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/macros.h"
#include "base/optional.h"
#include "services/ws/ids.h"
#include "services/ws/public/mojom/window_tree.mojom.h"
#include "services/ws/public/mojom/window_tree_constants.mojom.h"
#include "services/ws/window_tree.h"
#include "ui/gfx/geometry/rect.h"

namespace aura {
class Window;
}

namespace gfx {
class Insets;
class Transform;
}

namespace ui {

namespace mojom {
class WindowTree;

enum class EventTargetingPolicy;
}  // namespace mojom

}  // namespace ui

namespace ws {

class Embedding;

// Used for accessing private members of WindowTree in tests.
class WindowTreeTestHelper {
 public:
  explicit WindowTreeTestHelper(WindowTree* window_tree);
  ~WindowTreeTestHelper();

  mojom::WindowTree* window_tree();

  ClientSpecificId client_id() const { return window_tree_->client_id_; }

  mojom::WindowDataPtr WindowToWindowData(aura::Window* window);

  aura::Window* NewWindow(
      Id transport_window_id = 0,
      base::flat_map<std::string, std::vector<uint8_t>> properties = {});
  void DeleteWindow(aura::Window* window);
  aura::Window* NewTopLevelWindow(
      Id transport_window_id = 0,
      base::flat_map<std::string, std::vector<uint8_t>> properties = {});
  aura::Window* NewTopLevelWindow(
      const base::flat_map<std::string, std::vector<uint8_t>>& properties);
  bool SetCapture(aura::Window* window);
  bool ReleaseCapture(aura::Window* window);
  bool ReorderWindow(aura::Window* window,
                     aura::Window* relative_window,
                     mojom::OrderDirection direction);
  bool SetTransform(aura::Window* window, const gfx::Transform& transform);
  bool SetWindowBounds(aura::Window* window,
                       const gfx::Rect& bounds,
                       const base::Optional<viz::LocalSurfaceIdAllocation>&
                           local_surface_id_allocation =
                               base::Optional<viz::LocalSurfaceIdAllocation>());
  // Same as SetWindowBounds(), but called in such a way that the ack
  // (OnChangeCompleted()) is called on the client.
  void SetWindowBoundsWithAck(
      aura::Window* window,
      const gfx::Rect& bounds,
      const base::Optional<viz::LocalSurfaceIdAllocation>&
          local_surface_id_allocation =
              base::Optional<viz::LocalSurfaceIdAllocation>(),
      uint32_t change_id = 1);
  void SetClientArea(
      aura::Window* window,
      const gfx::Insets& insets,
      base::Optional<std::vector<gfx::Rect>> additional_client_areas =
          base::Optional<std::vector<gfx::Rect>>());
  void SetHitTestInsets(aura::Window* window,
                        const gfx::Insets& mouse,
                        const gfx::Insets& touch);
  bool SetWindowVisibility(aura::Window* window, bool visible);
  void SetWindowProperty(aura::Window* window,
                         const std::string& name,
                         const std::vector<uint8_t>& value,
                         uint32_t change_id = 1);

  // Creates a new embedding. On success the new Embedding is returned. The
  // returned Embedding is owned by the ProxyWindow for |window|.
  Embedding* Embed(aura::Window* window,
                   mojom::WindowTreeClientPtr client_ptr,
                   mojom::WindowTreeClient* client,
                   uint32_t embed_flags = 0);
  void SetEventTargetingPolicy(aura::Window* window,
                               mojom::EventTargetingPolicy policy);
  bool SetFocus(aura::Window* window);
  void SetCanFocus(aura::Window* window, bool can_focus);
  void SetCursor(aura::Window* window, ui::Cursor cursor);
  void OnWindowInputEventAck(uint32_t event_id, mojom::EventResult result);
  bool StackAbove(aura::Window* above_window, aura::Window* below_window);
  bool StackAtTop(aura::Window* window);

  Id TransportIdForWindow(aura::Window* window);

  void DestroyEmbedding(Embedding* embedding);

  aura::Window* GetWindowByClientId(const ClientWindowId& id);
  ClientWindowId ClientWindowIdForWindow(aura::Window* window);

  const std::queue<std::unique_ptr<WindowTree::InFlightEvent>>&
  in_flight_key_events() {
    return window_tree_->in_flight_key_events_;
  }
  const std::queue<std::unique_ptr<WindowTree::InFlightEvent>>&
  in_flight_other_events() {
    return window_tree_->in_flight_other_events_;
  }

 private:
  WindowTree* window_tree_;

  // Next id to use for creating a window (including top-level windows).
  Id next_window_id_ = 1;

  DISALLOW_COPY_AND_ASSIGN(WindowTreeTestHelper);
};

}  // namespace ws

#endif  // SERVICES_WS_WINDOW_TREE_TEST_HELPER_H_
