// Copyright (c) 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_THIRD_PARTY_QUIC_CORE_QUIC_VERSION_MANAGER_H_
#define NET_THIRD_PARTY_QUIC_CORE_QUIC_VERSION_MANAGER_H_

#include "net/third_party/quic/core/quic_versions.h"
#include "net/third_party/quic/platform/api/quic_export.h"

namespace quic {

// Used to generate filtered supported versions based on flags.
class QUIC_EXPORT_PRIVATE QuicVersionManager {
 public:
  explicit QuicVersionManager(ParsedQuicVersionVector supported_versions);
  virtual ~QuicVersionManager();

  // Returns currently supported QUIC versions.
  // TODO(nharper): Remove this method once it is unused.
  const QuicTransportVersionVector& GetSupportedTransportVersions();

  // Returns currently supported QUIC versions.
  const ParsedQuicVersionVector& GetSupportedVersions();

 protected:
  // Maybe refilter filtered_supported_versions_ based on flags.
  void MaybeRefilterSupportedVersions();

  // Refilters filtered_supported_versions_.
  virtual void RefilterSupportedVersions();

  const QuicTransportVersionVector& filtered_supported_versions() const {
    return filtered_transport_versions_;
  }

 private:
  // quic_enable_version_99 flag
  bool enable_version_99_;
  // quic_enable_version_47 flag
  bool enable_version_47_;
  // quic_enable_version_46 flag
  bool enable_version_46_;
  // quic_enable_version_44 flag
  bool enable_version_44_;
  // quic_enable_version_43 flag
  bool enable_version_43_;
  // quic_disable_version_39 flag
  bool disable_version_39_;
  // The list of versions that may be supported.
  ParsedQuicVersionVector allowed_supported_versions_;
  // This vector contains QUIC versions which are currently supported based on
  // flags.
  ParsedQuicVersionVector filtered_supported_versions_;
  // This vector contains the transport versions from
  // |filtered_supported_versions_|. No guarantees are made that the same
  // transport version isn't repeated.
  QuicTransportVersionVector filtered_transport_versions_;
};

}  // namespace quic

#endif  // NET_THIRD_PARTY_QUIC_CORE_QUIC_VERSION_MANAGER_H_
