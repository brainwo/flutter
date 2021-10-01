// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flatland_connection.h"

#include <zircon/status.h>

#include "flutter/fml/logging.h"

namespace flutter_runner {

FlatlandConnection::FlatlandConnection(
    std::string debug_label,
    fidl::InterfaceHandle<fuchsia::ui::composition::Flatland> flatland,
    fml::closure error_callback,
    on_frame_presented_event on_frame_presented_callback,
    uint64_t max_frames_in_flight,
    fml::TimeDelta vsync_offset)
    : flatland_(flatland.Bind()),
      error_callback_(error_callback),
      on_frame_presented_callback_(std::move(on_frame_presented_callback)) {
  flatland_.set_error_handler([callback = error_callback_](zx_status_t status) {
    FML_LOG(ERROR) << "Flatland disconnected" << zx_status_get_string(status);
    callback();
  });
  flatland_->SetDebugName(debug_label);
  flatland_.events().OnError =
      fit::bind_member(this, &FlatlandConnection::OnError);
  flatland_.events().OnFramePresented =
      fit::bind_member(this, &FlatlandConnection::OnFramePresented);
  flatland_.events().OnNextFrameBegin =
      fit::bind_member(this, &FlatlandConnection::OnNextFrameBegin);
}

FlatlandConnection::~FlatlandConnection() = default;

void FlatlandConnection::Present() {
  // TODO(fxbug.dev/64201): Consider a more complex presentation loop that
  // accumulates Present calls until OnNextFrameBegin.
  if (present_credits_ == 0)
    return;

  --present_credits_;
  fuchsia::ui::composition::PresentArgs present_args;
  present_args.set_requested_presentation_time(0);
  present_args.set_acquire_fences(std::move(acquire_fences_));
  present_args.set_release_fences(std::move(previous_present_release_fences_));
  present_args.set_unsquashable(false);
  flatland_->Present(std::move(present_args));

  // In Flatland, release fences apply to the content of the previous present.
  // Keeping track of the old frame's release fences and swapping ensure we set
  // the correct ones for VulkanSurface's interpretation.
  previous_present_release_fences_.clear();
  previous_present_release_fences_.swap(current_present_release_fences_);
  acquire_fences_.clear();
}

void FlatlandConnection::AwaitVsync(FireCallbackCallback callback) {
  if (first_call) {
    fml::TimePoint now = fml::TimePoint::Now();
    callback(now, now + kDefaultFlatlandPresentationInterval);
    first_call = false;
    return;
  }
  fire_callback_ = callback;
}

void FlatlandConnection::AwaitVsyncForSecondaryCallback(
    FireCallbackCallback callback) {}

void FlatlandConnection::OnError(
    fuchsia::ui::composition::FlatlandError error) {
  FML_LOG(ERROR) << "Flatland error: " << static_cast<int>(error);
  error_callback_();
}

void FlatlandConnection::OnNextFrameBegin(
    fuchsia::ui::composition::OnNextFrameBeginValues values) {
  present_credits_ += values.additional_present_credits();

  if (fire_callback_) {
    fml::TimePoint now = fml::TimePoint::Now();
    // TODO(fxbug.dev/64201): Calculate correct frame times.
    fire_callback_(now, now + kDefaultFlatlandPresentationInterval);
  }
}

void FlatlandConnection::OnFramePresented(
    fuchsia::scenic::scheduling::FramePresentedInfo info) {
  on_frame_presented_callback_(std::move(info));
}

void FlatlandConnection::EnqueueAcquireFence(zx::event fence) {
  acquire_fences_.push_back(std::move(fence));
}

void FlatlandConnection::EnqueueReleaseFence(zx::event fence) {
  current_present_release_fences_.push_back(std::move(fence));
}

}  // namespace flutter_runner
