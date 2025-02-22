// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/content_capture/renderer/content_capture_sender.h"

#include "components/content_capture/common/content_capture_data.h"
#include "components/content_capture/common/content_capture_features.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/web/web_content_holder.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"

namespace content_capture {

ContentCaptureSender::ContentCaptureSender(content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {
  render_frame->GetWebFrame()->SetContentCaptureClient(this);
}

ContentCaptureSender::~ContentCaptureSender() {}

cc::NodeHolder::Type ContentCaptureSender::GetNodeHolderType() const {
  if (content_capture::features::ShouldUseNodeID())
    return cc::NodeHolder::Type::kID;
  else
    return cc::NodeHolder::Type::kTextHolder;
}

void ContentCaptureSender::GetTaskTimingParameters(
    base::TimeDelta& short_delay,
    base::TimeDelta& long_delay) const {
  short_delay = base::TimeDelta::FromMilliseconds(
      features::TaskShortDelayInMilliseconds());
  long_delay = base::TimeDelta::FromMilliseconds(
      features::TaskLongDelayInMilliseconds());
}

void ContentCaptureSender::DidCaptureContent(
    const std::vector<scoped_refptr<blink::WebContentHolder>>& data,
    bool first_data) {
  ContentCaptureData frame_data;
  FillContentCaptureData(&frame_data, first_data /* set_url */);

  for (auto holder : data) {
    ContentCaptureData child;
    child.id = holder->GetId();
    child.value = holder->GetValue().Utf16();
    child.bounds = holder->GetBoundingBox();
    frame_data.children.push_back(child);
  }
  GetContentCaptureReceiver()->DidCaptureContent(frame_data, first_data);
}

void ContentCaptureSender::DidRemoveContent(const std::vector<int64_t>& data) {
  GetContentCaptureReceiver()->DidRemoveContent(data);
}

void ContentCaptureSender::OnDestruct() {
  base::ThreadTaskRunnerHandle::Get()->DeleteSoon(FROM_HERE, this);
}

void ContentCaptureSender::FillContentCaptureData(ContentCaptureData* data,
                                                  bool set_url) {
  data->bounds = render_frame()->GetWebFrame()->VisibleContentRect();
  if (set_url) {
    data->value =
        render_frame()->GetWebFrame()->GetDocument().Url().GetString().Utf16();
  }
}

const mojom::ContentCaptureReceiverAssociatedPtr&
ContentCaptureSender::GetContentCaptureReceiver() {
  if (!content_capture_receiver_) {
    render_frame()->GetRemoteAssociatedInterfaces()->GetInterface(
        mojo::MakeRequest(&content_capture_receiver_));
  }
  return content_capture_receiver_;
}

}  // namespace content_capture
