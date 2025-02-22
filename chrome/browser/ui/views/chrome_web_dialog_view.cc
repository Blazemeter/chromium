// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/chrome_web_dialog_view.h"

#include <memory>

#include "build/build_config.h"
#include "chrome/browser/extensions/chrome_extension_web_contents_observer.h"
#include "chrome/browser/ui/webui/chrome_web_contents_handler.h"
#include "ui/views/controls/webview/web_dialog_view.h"
#include "ui/views/widget/widget.h"

#if defined(OS_CHROMEOS)
#include "ash/public/cpp/shell_window_ids.h"
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ash/ash_util.h"
#include "chrome/browser/ui/ash/multi_user/multi_user_window_manager_client.h"
#include "components/user_manager/user.h"
#endif  // defined(OS_CHROMEOS)

namespace chrome {
namespace {

gfx::NativeWindow ShowWebDialogWidget(const views::Widget::InitParams& params,
                                      views::WebDialogView* view) {
  views::Widget* widget = new views::Widget;
  widget->Init(params);

  // Observer is needed for ChromeVox extension to send messages between content
  // and background scripts.
  extensions::ChromeExtensionWebContentsObserver::CreateForWebContents(
      view->web_contents());

  widget->Show();
  return widget->GetNativeWindow();
}

}  // namespace

// Declared in browser_dialogs.h so that others don't need to depend on our .h.
gfx::NativeWindow ShowWebDialog(gfx::NativeView parent,
                                content::BrowserContext* context,
                                ui::WebDialogDelegate* delegate) {
  return ShowWebDialogWithParams(parent, context, delegate, nullptr);
}

gfx::NativeWindow ShowWebDialogWithParams(
    gfx::NativeView parent,
    content::BrowserContext* context,
    ui::WebDialogDelegate* delegate,
    const views::Widget::InitParams* extra_params) {
  views::WebDialogView* view = new views::WebDialogView(
      context, delegate, std::make_unique<ChromeWebContentsHandler>());
  views::Widget::InitParams params;
  if (extra_params)
    params = *extra_params;
  params.delegate = view;
  params.parent = parent;
#if defined(OS_CHROMEOS)
  if (!parent && delegate->GetDialogModalType() == ui::MODAL_TYPE_SYSTEM) {
    int container_id = ash_util::GetSystemModalDialogContainerId();
    ash_util::SetupWidgetInitParamsForContainer(&params, container_id);
  }
#endif
  gfx::NativeWindow window = ShowWebDialogWidget(params, view);
#if defined(OS_CHROMEOS)
  const user_manager::User* user =
      chromeos::ProfileHelper::Get()->GetUserByProfile(
          Profile::FromBrowserContext(context));
  if (user) {
    // Dialogs should not be shown for other users when logged in.
    MultiUserWindowManagerClient::GetInstance()->SetWindowOwner(
        window, user->GetAccountId());
  }
#endif
  return window;
}

}  // namespace chrome
