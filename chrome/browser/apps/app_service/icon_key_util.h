// Copyright (c) 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_APPS_APP_SERVICE_ICON_KEY_UTIL_H_
#define CHROME_BROWSER_APPS_APP_SERVICE_ICON_KEY_UTIL_H_

// Utility classes for providing an App Service IconKey.

#include <string>

#include "base/macros.h"
#include "chrome/services/app_service/public/mojom/types.mojom.h"

namespace apps_util {

// Converts strings (such as App IDs) to IconKeys, such that passing the same
// string twice to MakeIconKey will result in different IconKeys (different not
// just in the pointer sense, but their IconKey.u_key values will also differ).
//
// Callers (which are presumably App Service app publishers) can therefore
// publish such IconKeys whenever an app's icon changes, even though the App ID
// itself doesn't change, and App Service app subscribers will notice (and
// reload) the new icon from the new (changed) icon key.
//
// The low 8 bits (a uint8_t) of the resultant IconKey's u_key are reserved for
// caller-specific flags. For example, colorful/gray icons for enabled/disabled
// states of the same app can be distinguished in one of those bits.
class IncrementingIconKeyFactory {
 public:
  IncrementingIconKeyFactory();

  apps::mojom::IconKeyPtr MakeIconKey(apps::mojom::AppType app_type,
                                      const std::string& s_key,
                                      uint8_t flags = 0);

 private:
  uint64_t u_key_;

  DISALLOW_COPY_AND_ASSIGN(IncrementingIconKeyFactory);
};

}  // namespace apps_util

#endif  // CHROME_BROWSER_APPS_APP_SERVICE_ICON_KEY_UTIL_H_
