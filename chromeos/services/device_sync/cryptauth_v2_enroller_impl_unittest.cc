// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/containers/flat_map.h"
#include "base/macros.h"
#include "base/no_destructor.h"
#include "base/stl_util.h"
#include "base/timer/mock_timer.h"
#include "chromeos/services/device_sync/cryptauth_constants.h"
#include "chromeos/services/device_sync/cryptauth_enrollment_result.h"
#include "chromeos/services/device_sync/cryptauth_key_creator_impl.h"
#include "chromeos/services/device_sync/cryptauth_key_proof_computer_impl.h"
#include "chromeos/services/device_sync/cryptauth_key_registry_impl.h"
#include "chromeos/services/device_sync/cryptauth_v2_enroller_impl.h"
#include "chromeos/services/device_sync/fake_cryptauth_key_creator.h"
#include "chromeos/services/device_sync/fake_cryptauth_key_proof_computer.h"
#include "chromeos/services/device_sync/mock_cryptauth_client.h"
#include "chromeos/services/device_sync/network_request_error.h"
#include "chromeos/services/device_sync/proto/cryptauth_better_together_feature_metadata.pb.h"
#include "chromeos/services/device_sync/proto/cryptauth_client_app_metadata.pb.h"
#include "chromeos/services/device_sync/proto/cryptauth_common.pb.h"
#include "chromeos/services/device_sync/proto/cryptauth_directive.pb.h"
#include "chromeos/services/device_sync/proto/cryptauth_enrollment.pb.h"
#include "chromeos/services/device_sync/public/cpp/gcm_constants.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromeos {

namespace device_sync {

using cryptauthv2::SyncKeysRequest;
using SyncSingleKeyRequest = cryptauthv2::SyncKeysRequest::SyncSingleKeyRequest;

using cryptauthv2::SyncKeysResponse;
using SyncSingleKeyResponse =
    cryptauthv2::SyncKeysResponse::SyncSingleKeyResponse;
using KeyAction =
    cryptauthv2::SyncKeysResponse::SyncSingleKeyResponse::KeyAction;
using KeyCreation =
    cryptauthv2::SyncKeysResponse::SyncSingleKeyResponse::KeyCreation;

using cryptauthv2::EnrollKeysRequest;
using EnrollSingleKeyRequest =
    cryptauthv2::EnrollKeysRequest::EnrollSingleKeyRequest;

using cryptauthv2::EnrollKeysResponse;
using EnrollSingleKeyResponse =
    cryptauthv2::EnrollKeysResponse::EnrollSingleKeyResponse;

using cryptauthv2::ApplicationSpecificMetadata;
using cryptauthv2::BetterTogetherFeatureMetadata;
using cryptauthv2::ClientAppMetadata;
using cryptauthv2::ClientDirective;
using cryptauthv2::ClientMetadata;
using cryptauthv2::FeatureMetadata;
using cryptauthv2::InvokeNext;
using cryptauthv2::KeyDirective;
using cryptauthv2::KeyType;
using cryptauthv2::PolicyReference;
using cryptauthv2::TargetService;

namespace {

const char kAccessTokenUsed[] = "access token used by CryptAuthClient";

const char kRandomSessionId[] = "random_session_id";

const char kOldActivePublicKey[] = "old_active_public_key";
const char kOldActivePrivateKey[] = "old_active_private_key";
const char kOldActiveAsymmetricKeyHandle[] = "old_active_handle";
const CryptAuthKey kOldActiveAsymmetricKey(kOldActivePublicKey,
                                           kOldActivePrivateKey,
                                           CryptAuthKey::Status::kActive,
                                           KeyType::P256,
                                           kOldActiveAsymmetricKeyHandle);

const char kOldInactivePublicKey[] = "old_inactive_public_key";
const char kOldInactivePrivateKey[] = "old_inactive_private_key";
const char kOldInactiveAsymmetricKeyHandle[] = "old_inactive_handle";
const CryptAuthKey kOldInactiveAsymmetricKey(kOldInactivePublicKey,
                                             kOldInactivePrivateKey,
                                             CryptAuthKey::Status::kInactive,
                                             KeyType::P256,
                                             kOldInactiveAsymmetricKeyHandle);

const char kOldActiveSymmetricKeyMaterial[] = "old_active_symmetric_key";
const char kOldActiveSymmetricKeyHandle[] = "old_active_symmetric_key_handle";
CryptAuthKey kOldActiveSymmetricKey(kOldActiveSymmetricKeyMaterial,
                                    CryptAuthKey::Status::kActive,
                                    KeyType::RAW128,
                                    kOldActiveSymmetricKeyHandle);

const char kOldInactiveSymmetricKeyMaterial[] = "old_inactive_symmetric_key";
const char kOldInactiveSymmetricKeyHandle[] =
    "old_inactive_symmetric_key_handle";
CryptAuthKey kOldInactiveSymmetricKey(kOldInactiveSymmetricKeyMaterial,
                                      CryptAuthKey::Status::kInactive,
                                      KeyType::RAW256,
                                      kOldInactiveSymmetricKeyHandle);

const char kNewPublicKey[] = "new_public_key";
const char kNewPrivateKey[] = "new_private_key";
const char kFixedUserKeyPairHandle[] = "device_key";

const char kNewSymmetricKey[] = "new_symmetric_key";
const char kNewSymmetricKeyHandle[] = "new_symmetric_key_handle";

const char kServerEphemeralDh[] = "server_ephemeral_dh";
const char kClientDhPublicKey[] = "client_ephemeral_dh_public_key";
const char kClientDhPrivateKey[] = "client_ephemeral_dh_private_key";
const CryptAuthKey kClientEphemeralDh(kClientDhPublicKey,
                                      kClientDhPrivateKey,
                                      CryptAuthKey::Status::kActive,
                                      KeyType::P256);

class FakeCryptAuthKeyCreatorFactory : public CryptAuthKeyCreatorImpl::Factory {
 public:
  FakeCryptAuthKeyCreatorFactory() = default;

  ~FakeCryptAuthKeyCreatorFactory() override = default;

  FakeCryptAuthKeyCreator* instance() { return instance_; }

 private:
  // CryptAuthKeyCreatorImpl::Factory:
  std::unique_ptr<CryptAuthKeyCreator> BuildInstance() override {
    auto instance = std::make_unique<FakeCryptAuthKeyCreator>();
    instance_ = instance.get();

    return instance;
  }

  FakeCryptAuthKeyCreator* instance_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(FakeCryptAuthKeyCreatorFactory);
};

class FakeCryptAuthKeyProofComputerFactory
    : public CryptAuthKeyProofComputerImpl::Factory {
 public:
  FakeCryptAuthKeyProofComputerFactory() = default;

  ~FakeCryptAuthKeyProofComputerFactory() override = default;

  void set_should_return_null_key_proof(bool should_return_null_key_proof) {
    should_return_null_key_proof_ = should_return_null_key_proof;
  }

 private:
  // CryptAuthKeyProofComputerImpl::Factory:
  std::unique_ptr<CryptAuthKeyProofComputer> BuildInstance() override {
    auto instance = std::make_unique<FakeCryptAuthKeyProofComputer>();
    instance->set_should_return_null(should_return_null_key_proof_);
    return instance;
  }

  bool should_return_null_key_proof_ = false;

  DISALLOW_COPY_AND_ASSIGN(FakeCryptAuthKeyProofComputerFactory);
};

class SyncSingleKeyResponseData {
 public:
  SyncSingleKeyResponseData(
      const CryptAuthKeyBundle::Name& bundle_name,
      const CryptAuthKeyRegistry* key_registry,
      const base::flat_map<std::string, KeyAction>& handle_to_action_map,
      const KeyCreation& new_key_creation,
      const base::Optional<KeyType>& new_key_type,
      const base::Optional<KeyDirective>& new_key_directive)
      : bundle_name(bundle_name),
        single_response(GenerateResponse(key_registry,
                                         handle_to_action_map,
                                         new_key_creation,
                                         new_key_type,
                                         new_key_directive)) {}

  CryptAuthKeyBundle::Name bundle_name;
  SyncSingleKeyResponse single_response;

 private:
  SyncSingleKeyResponse GenerateResponse(
      const CryptAuthKeyRegistry* key_registry,
      const base::flat_map<std::string, KeyAction>& handle_to_action_map,
      const KeyCreation& new_key_creation,
      const base::Optional<KeyType>& new_key_type,
      const base::Optional<KeyDirective>& new_key_directive) {
    SyncSingleKeyResponse single_response;
    single_response.set_key_creation(new_key_creation);
    if (new_key_type)
      single_response.set_key_type(*new_key_type);
    if (new_key_directive)
      single_response.mutable_key_directive()->CopyFrom(*new_key_directive);

    // If there are no keys, we don't need to add key actions.
    const CryptAuthKeyBundle* bundle = key_registry->GetKeyBundle(bundle_name);
    if (!bundle || handle_to_action_map.empty())
      return single_response;

    // We assume the enroller populated SyncSingleKeyRequest::key_handles in
    // the same order as the key bundle's handle-to-key map. Populate
    // SyncSingleKeyResponse::key_actions with the same ordering. If a key
    // action for a handle is not specified in |handle_to_action| map, use
    // KEY_ACTION_UNSPECIFIED.
    for (const std::pair<std::string, CryptAuthKey>& handle_key_pair :
         bundle->handle_to_key_map()) {
      auto it = handle_to_action_map.find(handle_key_pair.first);
      KeyAction key_action = it == handle_to_action_map.end()
                                 ? SyncSingleKeyResponse::KEY_ACTION_UNSPECIFIED
                                 : it->second;
      single_response.add_key_actions(key_action);
    }

    return single_response;
  }
};

ClientMetadata GetSampleClientMetadata() {
  ClientMetadata metadata;
  metadata.set_retry_count(2);
  metadata.set_invocation_reason(ClientMetadata::PERIODIC);

  return metadata;
}

ClientAppMetadata GetSampleClientAppMetadata() {
  ApplicationSpecificMetadata app_specific_metadata;
  app_specific_metadata.set_gcm_registration_id("GCM Registration ID");
  app_specific_metadata.set_device_software_package(kCryptAuthGcmAppId);

  BetterTogetherFeatureMetadata beto_metadata;
  beto_metadata.add_supported_features(
      BetterTogetherFeatureMetadata::BETTER_TOGETHER_CLIENT);
  beto_metadata.add_supported_features(
      BetterTogetherFeatureMetadata::SMS_CONNECT_CLIENT);

  FeatureMetadata feature_metadata;
  feature_metadata.set_feature_type(FeatureMetadata::BETTER_TOGETHER);
  feature_metadata.set_metadata(beto_metadata.SerializeAsString());

  ClientAppMetadata metadata;
  metadata.add_application_specific_metadata()->CopyFrom(app_specific_metadata);
  metadata.set_instance_id("Instance ID");
  metadata.set_instance_id_token("Instance ID Token");
  metadata.set_long_device_id("Long Device ID");
  metadata.add_feature_metadata()->CopyFrom(feature_metadata);

  return metadata;
}

PolicyReference GetSamplePreviousClientDirectivePolicyReference() {
  PolicyReference policy_reference;
  policy_reference.set_name("Previous Client Directive Policy Reference");
  policy_reference.set_version(1);

  return policy_reference;
}

ClientDirective GetSampleNewClientDirective() {
  PolicyReference policy_reference;
  policy_reference.set_name("New Client Directive Policy Reference");
  policy_reference.set_version(2);

  InvokeNext invoke_next;
  invoke_next.set_service(TargetService::DEVICE_SYNC);
  invoke_next.set_key_name("Target Service Key Name");

  ClientDirective client_directive;
  client_directive.mutable_policy_reference()->CopyFrom(policy_reference);
  client_directive.set_checkin_delay_millis(5000);
  client_directive.set_retry_attempts(3);
  client_directive.set_retry_period_millis(1000);
  client_directive.set_create_time_millis(1566073800000);
  client_directive.add_invoke_next()->CopyFrom(invoke_next);

  return client_directive;
}

KeyDirective GetSampleOldKeyDirective() {
  PolicyReference policy_reference;
  policy_reference.set_name("Old Key Policy Name");
  policy_reference.set_version(10);

  KeyDirective key_directive;
  key_directive.mutable_policy_reference()->CopyFrom(policy_reference);
  key_directive.set_enroll_time_millis(100);

  return key_directive;
}

KeyDirective GetSampleNewKeyDirective() {
  PolicyReference policy_reference;
  policy_reference.set_name("New Key Policy Name");
  policy_reference.set_version(20);

  KeyDirective key_directive;
  key_directive.mutable_policy_reference()->CopyFrom(policy_reference);
  key_directive.set_enroll_time_millis(200);

  return key_directive;
}

// Note: Copied from the implementation file.
const std::vector<CryptAuthKeyBundle::Name>& GetKeyBundleOrder() {
  static const base::NoDestructor<std::vector<CryptAuthKeyBundle::Name>> order(
      [] {
        std::vector<CryptAuthKeyBundle::Name> order;
        for (const CryptAuthKeyBundle::Name& bundle_name :
             CryptAuthKeyBundle::AllNames())
          order.push_back(bundle_name);
        return order;
      }());

  return *order;
}

// Returns the index of SyncKeysRequest.sync_single_key_requests or
// SyncKeysResponse.sync_single_key_responses that contains information about
// the key bundle |bundle_name|.
size_t GetKeyBundleIndex(const CryptAuthKeyBundle::Name& bundle_name) {
  for (size_t i = 0; i < GetKeyBundleOrder().size(); ++i) {
    if (GetKeyBundleOrder()[i] == bundle_name)
      return i;
  }

  return GetKeyBundleOrder().size();
}

// Builds a SyncKeysResponse, ensuring that the SyncSingleKeyResponses ordering
// aligns with GetKeyBundleOrder().
SyncKeysResponse BuildSyncKeysResponse(
    std::vector<SyncSingleKeyResponseData> sync_single_key_responses_data = {},
    const std::string& session_id = kRandomSessionId,
    const std::string& server_ephemeral_dh = kServerEphemeralDh,
    const ClientDirective& client_directive = GetSampleNewClientDirective()) {
  SyncKeysResponse sync_keys_response;
  sync_keys_response.set_random_session_id(session_id);
  sync_keys_response.set_server_ephemeral_dh(server_ephemeral_dh);
  sync_keys_response.mutable_client_directive()->CopyFrom(client_directive);

  // Make sure there are at least as many SyncSingleKeyResponses as key bundles.
  while (
      static_cast<size_t>(sync_keys_response.sync_single_key_responses_size()) <
      GetKeyBundleOrder().size()) {
    sync_keys_response.add_sync_single_key_responses();
  }

  // Populate the relevant SyncSingleKeyResponse for each key bundle with data
  // from the input |sync_single_key_responses_map|.
  for (const SyncSingleKeyResponseData& data : sync_single_key_responses_data) {
    sync_keys_response
        .mutable_sync_single_key_responses(GetKeyBundleIndex(data.bundle_name))
        ->CopyFrom(data.single_response);
  }

  return sync_keys_response;
}

}  // namespace

class DeviceSyncCryptAuthV2EnrollerImplTest
    : public testing::Test,
      public MockCryptAuthClientFactory::Observer {
 protected:
  DeviceSyncCryptAuthV2EnrollerImplTest()
      : client_factory_(std::make_unique<MockCryptAuthClientFactory>(
            MockCryptAuthClientFactory::MockType::MAKE_NICE_MOCKS)),
        fake_cryptauth_key_creator_factory_(
            std::make_unique<FakeCryptAuthKeyCreatorFactory>()),
        fake_cryptauth_key_proof_computer_factory_(
            std::make_unique<FakeCryptAuthKeyProofComputerFactory>()) {
    CryptAuthKeyRegistryImpl::RegisterPrefs(pref_service_.registry());
    key_registry_ =
        CryptAuthKeyRegistryImpl::Factory::Get()->BuildInstance(&pref_service_);

    client_factory_->AddObserver(this);
  }

  ~DeviceSyncCryptAuthV2EnrollerImplTest() override {
    client_factory_->RemoveObserver(this);
  }

  // testing::Test:
  void SetUp() override {
    CryptAuthKeyCreatorImpl::Factory::SetFactoryForTesting(
        fake_cryptauth_key_creator_factory_.get());
    CryptAuthKeyProofComputerImpl::Factory::SetFactoryForTesting(
        fake_cryptauth_key_proof_computer_factory_.get());

    auto mock_timer = std::make_unique<base::MockOneShotTimer>();
    timer_ = mock_timer.get();

    enroller_ = CryptAuthV2EnrollerImpl::Factory::Get()->BuildInstance(
        key_registry(), client_factory(), std::move(mock_timer));
  }

  // testing::Test:
  void TearDown() override {
    CryptAuthKeyCreatorImpl::Factory::SetFactoryForTesting(nullptr);
    CryptAuthKeyProofComputerImpl::Factory::SetFactoryForTesting(nullptr);
  }

  // MockCryptAuthClientFactory::Observer:
  void OnCryptAuthClientCreated(MockCryptAuthClient* client) override {
    ON_CALL(*client, SyncKeys(testing::_, testing::_, testing::_))
        .WillByDefault(
            Invoke(this, &DeviceSyncCryptAuthV2EnrollerImplTest::OnSyncKeys));

    ON_CALL(*client, EnrollKeys(testing::_, testing::_, testing::_))
        .WillByDefault(
            Invoke(this, &DeviceSyncCryptAuthV2EnrollerImplTest::OnEnrollKeys));

    ON_CALL(*client, GetAccessTokenUsed())
        .WillByDefault(testing::Return(kAccessTokenUsed));
  }

  void CallEnroll(const cryptauthv2::ClientMetadata& client_metadata,
                  const cryptauthv2::ClientAppMetadata& client_app_metadata,
                  const base::Optional<cryptauthv2::PolicyReference>&
                      client_directive_policy_reference) {
    enroller()->Enroll(
        client_metadata, client_app_metadata, client_directive_policy_reference,
        base::BindOnce(
            &DeviceSyncCryptAuthV2EnrollerImplTest::OnEnrollmentComplete,
            base::Unretained(this)));
  }

  void OnSyncKeys(const SyncKeysRequest& request,
                  const CryptAuthClient::SyncKeysCallback& callback,
                  const CryptAuthClient::ErrorCallback& error_callback) {
    // Check that SyncKeys is called before EnrollKeys.
    EXPECT_FALSE(sync_keys_request_);
    EXPECT_FALSE(enroll_keys_request_);
    EXPECT_TRUE(sync_keys_success_callback_.is_null());
    EXPECT_TRUE(enroll_keys_success_callback_.is_null());
    EXPECT_TRUE(sync_keys_failure_callback_.is_null());
    EXPECT_TRUE(enroll_keys_failure_callback_.is_null());

    sync_keys_request_ = request;
    sync_keys_success_callback_ = callback;
    sync_keys_failure_callback_ = error_callback;
  }

  void SendSyncKeysResponse(const SyncKeysResponse& sync_keys_response) {
    std::move(sync_keys_success_callback_).Run(sync_keys_response);
  }

  void FailSyncKeysRequest(const NetworkRequestError& network_request_error) {
    std::move(sync_keys_failure_callback_).Run(network_request_error);
  }

  void RunKeyCreator(const base::flat_map<CryptAuthKeyBundle::Name,
                                          CryptAuthKey>& new_keys_output,
                     const CryptAuthKey& client_ephemeral_dh_output) {
    std::move(key_creator()->create_keys_callback())
        .Run(new_keys_output, client_ephemeral_dh_output);
  }

  void SendEnrollKeysResponse(const EnrollKeysResponse& enroll_keys_response) {
    std::move(enroll_keys_success_callback_).Run(enroll_keys_response);
  }

  void FailEnrollKeysRequest(const NetworkRequestError& network_request_error) {
    std::move(enroll_keys_failure_callback_).Run(network_request_error);
  }

  void OnEnrollKeys(const EnrollKeysRequest& request,
                    const CryptAuthClient::EnrollKeysCallback& callback,
                    const CryptAuthClient::ErrorCallback& error_callback) {
    // Check that EnrollKeys is called after a successful SyncKeys call.
    EXPECT_TRUE(sync_keys_request_);
    EXPECT_FALSE(enroll_keys_request_);
    EXPECT_TRUE(sync_keys_success_callback_.is_null());
    EXPECT_TRUE(enroll_keys_success_callback_.is_null());
    EXPECT_FALSE(sync_keys_failure_callback_.is_null());
    EXPECT_TRUE(enroll_keys_failure_callback_.is_null());

    enroll_keys_request_ = request;
    enroll_keys_success_callback_ = callback;
    enroll_keys_failure_callback_ = error_callback;
  }

  void VerifyKeyCreatorInputs(
      const base::flat_map<CryptAuthKeyBundle::Name, CryptAuthKey>&
          expected_new_keys,
      const std::string& expected_server_ephemeral_dh_public_key) {
    ASSERT_EQ(expected_new_keys.size(), key_creator()->keys_to_create().size());
    for (const std::pair<CryptAuthKeyBundle::Name, CryptAuthKey>&
             name_key_pair : expected_new_keys) {
      const CryptAuthKeyBundle::Name& bundle_name = name_key_pair.first;
      const CryptAuthKey& key = name_key_pair.second;
      ASSERT_TRUE(
          base::ContainsKey(key_creator()->keys_to_create(), bundle_name));
      const CryptAuthKeyCreator::CreateKeyData& create_key_data =
          key_creator()->keys_to_create().find(bundle_name)->second;

      EXPECT_EQ(key.status(), create_key_data.status);
      EXPECT_EQ(key.type(), create_key_data.type);
      if (bundle_name == CryptAuthKeyBundle::Name::kUserKeyPair)
        EXPECT_EQ(key.handle(), create_key_data.handle);
    }

    ASSERT_TRUE(key_creator()->server_ephemeral_dh()->IsAsymmetricKey());
    EXPECT_EQ(expected_server_ephemeral_dh_public_key,
              key_creator()->server_ephemeral_dh()->public_key());
    EXPECT_EQ(KeyType::P256, key_creator()->server_ephemeral_dh()->type());
  }

  CryptAuthV2Enroller* enroller() { return enroller_.get(); }

  CryptAuthKeyRegistry* key_registry() { return key_registry_.get(); }

  CryptAuthClientFactory* client_factory() { return client_factory_.get(); }

  FakeCryptAuthKeyProofComputerFactory* key_proof_computer_factory() {
    return fake_cryptauth_key_proof_computer_factory_.get();
  }

  base::MockOneShotTimer* timer() { return timer_; }

  const base::Optional<SyncKeysRequest>& sync_keys_request() {
    return sync_keys_request_;
  }

  const base::Optional<EnrollKeysRequest>& enroll_keys_request() {
    return enroll_keys_request_;
  }

  const base::Optional<CryptAuthEnrollmentResult>& enrollment_result() {
    return enrollment_result_;
  }

 private:
  void OnEnrollmentComplete(
      const CryptAuthEnrollmentResult& enrollment_result) {
    enrollment_result_ = enrollment_result;
  }

  FakeCryptAuthKeyCreator* key_creator() {
    return fake_cryptauth_key_creator_factory_->instance();
  }

  TestingPrefServiceSimple pref_service_;
  std::unique_ptr<CryptAuthKeyRegistry> key_registry_;
  std::unique_ptr<MockCryptAuthClientFactory> client_factory_;
  base::MockOneShotTimer* timer_;

  std::unique_ptr<FakeCryptAuthKeyCreatorFactory>
      fake_cryptauth_key_creator_factory_;
  std::unique_ptr<FakeCryptAuthKeyProofComputerFactory>
      fake_cryptauth_key_proof_computer_factory_;

  // Parameters passed to the CryptAuthClient functions {Sync,Enroll}Keys().
  base::Optional<SyncKeysRequest> sync_keys_request_;
  base::Optional<EnrollKeysRequest> enroll_keys_request_;
  CryptAuthClient::SyncKeysCallback sync_keys_success_callback_;
  CryptAuthClient::EnrollKeysCallback enroll_keys_success_callback_;
  CryptAuthClient::ErrorCallback sync_keys_failure_callback_;
  CryptAuthClient::ErrorCallback enroll_keys_failure_callback_;

  base::Optional<CryptAuthEnrollmentResult> enrollment_result_;

  std::unique_ptr<CryptAuthV2Enroller> enroller_;

  DISALLOW_COPY_AND_ASSIGN(DeviceSyncCryptAuthV2EnrollerImplTest);
};

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest, SuccessfulEnrollment) {
  // Seed key registry.
  key_registry()->AddEnrolledKey(CryptAuthKeyBundle::Name::kUserKeyPair,
                                 kOldActiveAsymmetricKey);
  key_registry()->AddEnrolledKey(CryptAuthKeyBundle::Name::kUserKeyPair,
                                 kOldInactiveAsymmetricKey);
  key_registry()->SetKeyDirective(CryptAuthKeyBundle::Name::kUserKeyPair,
                                  GetSampleOldKeyDirective());
  CryptAuthKeyBundle expected_key_bundle_user_key_pair(
      *key_registry()->GetKeyBundle(CryptAuthKeyBundle::Name::kUserKeyPair));

  key_registry()->AddEnrolledKey(CryptAuthKeyBundle::Name::kLegacyMasterKey,
                                 kOldActiveSymmetricKey);
  key_registry()->AddEnrolledKey(CryptAuthKeyBundle::Name::kLegacyMasterKey,
                                 kOldInactiveSymmetricKey);
  key_registry()->SetKeyDirective(CryptAuthKeyBundle::Name::kLegacyMasterKey,
                                  GetSampleOldKeyDirective());
  CryptAuthKeyBundle expected_key_bundle_legacy_master_key(
      *key_registry()->GetKeyBundle(
          CryptAuthKeyBundle::Name::kLegacyMasterKey));

  // Start the enrollment flow.
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  ClientDirective expected_new_client_directive = GetSampleNewClientDirective();
  KeyDirective expected_new_key_directive = GetSampleNewKeyDirective();

  // For kUserKeyPair:
  //   - active --> deleted
  //   - inactive --> temporarily active during key creation
  //   - new --> active after created
  // For kMasterLegacyKey:
  //   - active --> active
  //   - inactive --> inactive
  //   - new --> inactive
  std::vector<SyncSingleKeyResponseData> sync_single_key_responses_data = {
      SyncSingleKeyResponseData(
          CryptAuthKeyBundle::Name::kUserKeyPair, key_registry(),
          {{kOldActiveAsymmetricKeyHandle, SyncSingleKeyResponse::DELETE},
           {kOldInactiveAsymmetricKeyHandle,
            SyncSingleKeyResponse::ACTIVATE}} /* handle_to_action_map */,
          SyncSingleKeyResponse::ACTIVE /* new_key_creation */,
          KeyType::P256 /* new_key_type */,
          expected_new_key_directive /* new_key_directive */),
      SyncSingleKeyResponseData(
          CryptAuthKeyBundle::Name::kLegacyMasterKey, key_registry(),
          {{kOldActiveSymmetricKeyHandle, SyncSingleKeyResponse::ACTIVATE},
           {kOldInactiveSymmetricKeyHandle,
            SyncSingleKeyResponse::DEACTIVATE}} /* handle_to_action_map */,
          SyncSingleKeyResponse::INACTIVE /* new_key_creation */,
          KeyType::RAW256 /* new_key_type */,
          expected_new_key_directive /* new_key_directive */)};

  SyncKeysResponse sync_keys_response =
      BuildSyncKeysResponse(sync_single_key_responses_data, kRandomSessionId,
                            kServerEphemeralDh, expected_new_client_directive);

  // Assume a successful SyncKeys() call.
  SendSyncKeysResponse(sync_keys_response);

  // Verify that the key actions were applied. (Note: New keys not created yet.)
  expected_key_bundle_user_key_pair.DeleteKey(kOldActiveAsymmetricKeyHandle);
  expected_key_bundle_user_key_pair.SetActiveKey(
      kOldInactiveAsymmetricKeyHandle);
  EXPECT_EQ(
      expected_key_bundle_user_key_pair,
      *key_registry()->GetKeyBundle(CryptAuthKeyBundle::Name::kUserKeyPair));

  EXPECT_EQ(expected_key_bundle_legacy_master_key,
            *key_registry()->GetKeyBundle(
                CryptAuthKeyBundle::Name::kLegacyMasterKey));

  // Verify the key creation data, and assume successful key creation.
  base::flat_map<CryptAuthKeyBundle::Name, CryptAuthKey> expected_new_keys = {
      {CryptAuthKeyBundle::Name::kUserKeyPair,
       CryptAuthKey(kNewPublicKey, kNewPrivateKey,
                    CryptAuthKey::Status::kActive, KeyType::P256,
                    kFixedUserKeyPairHandle)},
      {CryptAuthKeyBundle::Name::kLegacyMasterKey,
       CryptAuthKey(kNewSymmetricKey, CryptAuthKey::Status::kInactive,
                    KeyType::RAW256, kNewSymmetricKeyHandle)}};

  VerifyKeyCreatorInputs(
      expected_new_keys,
      kServerEphemeralDh /* expected_server_ephemeral_dh_public_key */);

  RunKeyCreator(expected_new_keys, kClientEphemeralDh);

  // Verify EnrollKeysRequest.
  EXPECT_EQ(kRandomSessionId, enroll_keys_request()->random_session_id());
  EXPECT_EQ(kClientDhPublicKey, enroll_keys_request()->client_ephemeral_dh());
  EXPECT_EQ(2, enroll_keys_request()->enroll_single_key_requests_size());

  std::unique_ptr<CryptAuthKeyProofComputer> key_proof_computer =
      CryptAuthKeyProofComputerImpl::Factory::Get()->BuildInstance();

  const EnrollSingleKeyRequest& single_request_user_key_pair =
      enroll_keys_request()->enroll_single_key_requests(
          GetKeyBundleIndex(CryptAuthKeyBundle::Name::kUserKeyPair));
  EXPECT_EQ(CryptAuthKeyBundle::KeyBundleNameEnumToString(
                CryptAuthKeyBundle::Name::kUserKeyPair),
            single_request_user_key_pair.key_name());
  EXPECT_EQ(kFixedUserKeyPairHandle,
            single_request_user_key_pair.new_key_handle());
  EXPECT_EQ(kNewPublicKey, single_request_user_key_pair.key_material());
  EXPECT_EQ(key_proof_computer->ComputeKeyProof(
                expected_new_keys.find(CryptAuthKeyBundle::Name::kUserKeyPair)
                    ->second,
                kRandomSessionId, kCryptAuthKeyProofSalt),
            single_request_user_key_pair.key_proof());

  const EnrollSingleKeyRequest& single_request_legacy_master_key =
      enroll_keys_request()->enroll_single_key_requests(
          GetKeyBundleIndex(CryptAuthKeyBundle::Name::kLegacyMasterKey));
  EXPECT_EQ(CryptAuthKeyBundle::KeyBundleNameEnumToString(
                CryptAuthKeyBundle::Name::kLegacyMasterKey),
            single_request_legacy_master_key.key_name());
  EXPECT_EQ(kNewSymmetricKeyHandle,
            single_request_legacy_master_key.new_key_handle());
  EXPECT_TRUE(single_request_legacy_master_key.key_material().empty());
  EXPECT_EQ(
      key_proof_computer->ComputeKeyProof(
          expected_new_keys.find(CryptAuthKeyBundle::Name::kLegacyMasterKey)
              ->second,
          kRandomSessionId, kCryptAuthKeyProofSalt),
      single_request_legacy_master_key.key_proof());

  // Assume a successful EnrollKeys() call.
  // Note: No parameters in EnrollKeysResponse are processed by the enroller
  // (yet), so send a trivial response.
  SendEnrollKeysResponse(EnrollKeysResponse());

  // Verify enrollment result.
  EXPECT_EQ(CryptAuthEnrollmentResult(
                CryptAuthEnrollmentResult::ResultCode::kSuccessNewKeysEnrolled,
                expected_new_client_directive),
            enrollment_result());

  // Verify that the key registry is updated with the newly enrolled keys
  // and new key directives.
  CryptAuthKeyBundle::Name bundle_name = CryptAuthKeyBundle::Name::kUserKeyPair;
  expected_key_bundle_user_key_pair.AddKey(
      expected_new_keys.find(bundle_name)->second);
  expected_key_bundle_user_key_pair.set_key_directive(
      expected_new_key_directive);
  EXPECT_EQ(expected_key_bundle_user_key_pair,
            *key_registry()->GetKeyBundle(bundle_name));

  bundle_name = CryptAuthKeyBundle::Name::kLegacyMasterKey;
  expected_key_bundle_legacy_master_key.AddKey(
      expected_new_keys.find(bundle_name)->second);
  expected_key_bundle_legacy_master_key.set_key_directive(
      expected_new_key_directive);
  EXPECT_EQ(expected_key_bundle_legacy_master_key,
            *key_registry()->GetKeyBundle(bundle_name));
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest,
       SuccessfulEnrollment_NoKeysCreated) {
  key_registry()->AddEnrolledKey(CryptAuthKeyBundle::Name::kUserKeyPair,
                                 kOldActiveAsymmetricKey);
  key_registry()->AddEnrolledKey(CryptAuthKeyBundle::Name::kUserKeyPair,
                                 kOldInactiveAsymmetricKey);
  key_registry()->SetKeyDirective(CryptAuthKeyBundle::Name::kUserKeyPair,
                                  GetSampleOldKeyDirective());
  CryptAuthKeyBundle expected_key_bundle(
      *key_registry()->GetKeyBundle(CryptAuthKeyBundle::Name::kUserKeyPair));

  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  // Simulate CryptAuth instructing us to swap active and inactive key states
  // but not create any new keys.
  SyncKeysResponse sync_keys_response =
      BuildSyncKeysResponse({SyncSingleKeyResponseData(
          CryptAuthKeyBundle::Name::kUserKeyPair, key_registry(),
          {{kOldActiveAsymmetricKeyHandle, SyncSingleKeyResponse::DEACTIVATE},
           {kOldInactiveAsymmetricKeyHandle,
            SyncSingleKeyResponse::ACTIVATE}} /* handle_to_action_map */,
          SyncSingleKeyResponse::NONE /* new_key_creation */,
          base::nullopt /* new_key_type */,
          base::nullopt /* new_key_directive */)});
  SendSyncKeysResponse(sync_keys_response);

  expected_key_bundle.SetActiveKey(kOldInactiveAsymmetricKeyHandle);
  EXPECT_EQ(expected_key_bundle, *key_registry()->GetKeyBundle(
                                     CryptAuthKeyBundle::Name::kUserKeyPair));

  EXPECT_EQ(CryptAuthEnrollmentResult(
                CryptAuthEnrollmentResult::ResultCode::kSuccessNoNewKeysNeeded,
                sync_keys_response.client_directive()),
            enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest, Failure_ServerOverloaded) {
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  SyncKeysResponse sync_keys_response = BuildSyncKeysResponse();
  sync_keys_response.set_server_status(SyncKeysResponse::SERVER_OVERLOADED);

  SendSyncKeysResponse(sync_keys_response);

  EXPECT_EQ(CryptAuthEnrollmentResult(CryptAuthEnrollmentResult::ResultCode::
                                          kErrorCryptAuthServerOverloaded,
                                      base::nullopt /* client_directive */),
            enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest, Failure_MissingSessionId) {
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  SyncKeysResponse sync_keys_response = BuildSyncKeysResponse();
  sync_keys_response.release_random_session_id();

  SendSyncKeysResponse(sync_keys_response);

  EXPECT_EQ(CryptAuthEnrollmentResult(
                CryptAuthEnrollmentResult::ResultCode::
                    kErrorSyncKeysResponseMissingRandomSessionId,
                base::nullopt /* client_directive */),
            enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest, Failure_MissingClientDirective) {
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  SyncKeysResponse sync_keys_response = BuildSyncKeysResponse();
  sync_keys_response.release_client_directive();

  SendSyncKeysResponse(sync_keys_response);

  EXPECT_EQ(CryptAuthEnrollmentResult(
                CryptAuthEnrollmentResult::ResultCode::
                    kErrorSyncKeysResponseInvalidClientDirective,
                base::nullopt /* client_directive */),
            enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest,
       Failure_InvalidSyncSingleKeyResponsesSize) {
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  SyncKeysResponse sync_keys_response = BuildSyncKeysResponse();
  sync_keys_response.clear_sync_single_key_responses();

  SendSyncKeysResponse(sync_keys_response);

  EXPECT_EQ(
      CryptAuthEnrollmentResult(CryptAuthEnrollmentResult::ResultCode::
                                    kErrorWrongNumberOfSyncSingleKeyResponses,
                                base::nullopt /* client_directive */),
      enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest, Failure_InvalidKeyActions_Size) {
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  SyncKeysResponse sync_keys_response = BuildSyncKeysResponse();

  // Add a key action for a bundle that has no keys.
  sync_keys_response.mutable_sync_single_key_responses(0)->add_key_actions(
      SyncSingleKeyResponse::ACTIVATE);

  SendSyncKeysResponse(sync_keys_response);

  EXPECT_EQ(
      CryptAuthEnrollmentResult(
          CryptAuthEnrollmentResult::ResultCode::kErrorWrongNumberOfKeyActions,
          sync_keys_response.client_directive()),
      enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest,
       Failure_InvalidKeyActions_NoActiveKey) {
  key_registry()->AddEnrolledKey(CryptAuthKeyBundle::Name::kUserKeyPair,
                                 kOldActiveAsymmetricKey);

  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  // Try to deactivate the only active key.
  SyncKeysResponse sync_keys_response =
      BuildSyncKeysResponse({SyncSingleKeyResponseData(
          CryptAuthKeyBundle::Name::kUserKeyPair, key_registry(),
          {{kOldActiveAsymmetricKeyHandle,
            SyncSingleKeyResponse::DEACTIVATE}} /* handle_to_action_map */,
          SyncSingleKeyResponse::NONE /* new_key_creation */,
          base::nullopt /* new_key_type */,
          base::nullopt /* new_key_directive */)});
  SendSyncKeysResponse(sync_keys_response);

  EXPECT_EQ(
      CryptAuthEnrollmentResult(CryptAuthEnrollmentResult::ResultCode::
                                    kErrorKeyActionsDoNotSpecifyAnActiveKey,
                                sync_keys_response.client_directive()),
      enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest,
       Failure_InvalidKeyCreationInstructions_UnsupportedKeyType) {
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  // Instruct client to create an unsupported key type, CURVE25519.
  SyncKeysResponse sync_keys_response =
      BuildSyncKeysResponse({SyncSingleKeyResponseData(
          CryptAuthKeyBundle::Name::kUserKeyPair, key_registry(),
          {} /* handle_to_action_map */,
          SyncSingleKeyResponse::ACTIVE /* new_key_creation */,
          KeyType::CURVE25519 /* new_key_type */,
          base::nullopt /* new_key_directive */)});
  SendSyncKeysResponse(sync_keys_response);

  EXPECT_EQ(CryptAuthEnrollmentResult(CryptAuthEnrollmentResult::ResultCode::
                                          kErrorKeyCreationKeyTypeNotSupported,
                                      sync_keys_response.client_directive()),
            enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest,
       Failure_InvalidKeyCreationInstructions_NoServerDiffieHellman) {
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  SyncKeysResponse sync_keys_response =
      BuildSyncKeysResponse({SyncSingleKeyResponseData(
          CryptAuthKeyBundle::Name::kUserKeyPair, key_registry(),
          {} /* handle_to_action_map */,
          SyncSingleKeyResponse::ACTIVE /* new_key_creation */,
          KeyType::RAW256 /* new_key_type */,
          base::nullopt /* new_key_directive */)});
  sync_keys_response.release_server_ephemeral_dh();

  SendSyncKeysResponse(sync_keys_response);

  EXPECT_EQ(CryptAuthEnrollmentResult(
                CryptAuthEnrollmentResult::ResultCode::
                    kErrorSymmetricKeyCreationMissingServerDiffieHellman,
                sync_keys_response.client_directive()),
            enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest,
       Failure_KeyProofComputationFailed) {
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  SyncKeysResponse sync_keys_response =
      BuildSyncKeysResponse({SyncSingleKeyResponseData(
          CryptAuthKeyBundle::Name::kUserKeyPair, key_registry(),
          {} /* handle_to_action_map */,
          SyncSingleKeyResponse::ACTIVE /* new_key_creation */,
          KeyType::P256 /* new_key_type */,
          base::nullopt /* new_key_directive */)});
  SendSyncKeysResponse(sync_keys_response);

  key_proof_computer_factory()->set_should_return_null_key_proof(true);

  base::flat_map<CryptAuthKeyBundle::Name, CryptAuthKey> expected_new_keys = {
      {CryptAuthKeyBundle::Name::kUserKeyPair,
       CryptAuthKey(kNewPublicKey, kNewPrivateKey,
                    CryptAuthKey::Status::kActive, KeyType::P256,
                    kFixedUserKeyPairHandle)}};
  RunKeyCreator(expected_new_keys, kClientEphemeralDh);

  EXPECT_EQ(CryptAuthEnrollmentResult(CryptAuthEnrollmentResult::ResultCode::
                                          kErrorKeyProofComputationFailed,
                                      sync_keys_response.client_directive()),
            enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest, Failure_SyncKeysApiCall) {
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  FailSyncKeysRequest(NetworkRequestError::kAuthenticationError);

  EXPECT_EQ(
      CryptAuthEnrollmentResult(CryptAuthEnrollmentResult::ResultCode::
                                    kErrorSyncKeysApiCallAuthenticationError,
                                base::nullopt /* client_directive */),
      enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest, Failure_EnrollKeysApiCall) {
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  SyncKeysResponse sync_keys_response =
      BuildSyncKeysResponse({SyncSingleKeyResponseData(
          CryptAuthKeyBundle::Name::kUserKeyPair, key_registry(),
          {} /* handle_to_action_map */,
          SyncSingleKeyResponse::ACTIVE /* new_key_creation */,
          KeyType::P256 /* new_key_type */,
          base::nullopt /* new_key_directive */)});
  SendSyncKeysResponse(sync_keys_response);

  base::flat_map<CryptAuthKeyBundle::Name, CryptAuthKey> expected_new_keys = {
      {CryptAuthKeyBundle::Name::kUserKeyPair,
       CryptAuthKey(kNewPublicKey, kNewPrivateKey,
                    CryptAuthKey::Status::kActive, KeyType::P256,
                    kFixedUserKeyPairHandle)}};
  RunKeyCreator(expected_new_keys, kClientEphemeralDh);

  FailEnrollKeysRequest(NetworkRequestError::kBadRequest);

  EXPECT_EQ(CryptAuthEnrollmentResult(CryptAuthEnrollmentResult::ResultCode::
                                          kErrorEnrollKeysApiCallBadRequest,
                                      sync_keys_response.client_directive()),
            enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest,
       Failure_Timeout_WaitingForSyncKeysResponse) {
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  // Timeout waiting for SyncKeysResponse.
  EXPECT_TRUE(timer()->IsRunning());
  timer()->Fire();

  EXPECT_EQ(
      CryptAuthEnrollmentResult(CryptAuthEnrollmentResult::ResultCode::
                                    kErrorTimeoutWaitingForSyncKeysResponse,
                                base::nullopt /* client_directive */),
      enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest,
       Failure_Timeout_WaitingForKeyCreation) {
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  SyncKeysResponse sync_keys_response =
      BuildSyncKeysResponse({SyncSingleKeyResponseData(
          CryptAuthKeyBundle::Name::kUserKeyPair, key_registry(),
          {} /* handle_to_action_map */,
          SyncSingleKeyResponse::ACTIVE /* new_key_creation */,
          KeyType::P256 /* new_key_type */,
          base::nullopt /* new_key_directive */)});
  SendSyncKeysResponse(sync_keys_response);

  // Timeout waiting for key creation.
  EXPECT_TRUE(timer()->IsRunning());
  timer()->Fire();

  EXPECT_EQ(CryptAuthEnrollmentResult(CryptAuthEnrollmentResult::ResultCode::
                                          kErrorTimeoutWaitingForKeyCreation,
                                      sync_keys_response.client_directive()),
            enrollment_result());
}

TEST_F(DeviceSyncCryptAuthV2EnrollerImplTest,
       Failure_Timeout_WaitingForEnrollKeysResponse) {
  CallEnroll(GetSampleClientMetadata(), GetSampleClientAppMetadata(),
             GetSamplePreviousClientDirectivePolicyReference());

  SyncKeysResponse sync_keys_response =
      BuildSyncKeysResponse({SyncSingleKeyResponseData(
          CryptAuthKeyBundle::Name::kUserKeyPair, key_registry(),
          {} /* handle_to_action_map */,
          SyncSingleKeyResponse::ACTIVE /* new_key_creation */,
          KeyType::P256 /* new_key_type */,
          base::nullopt /* new_key_directive */)});
  SendSyncKeysResponse(sync_keys_response);

  base::flat_map<CryptAuthKeyBundle::Name, CryptAuthKey> expected_new_keys = {
      {CryptAuthKeyBundle::Name::kUserKeyPair,
       CryptAuthKey(kNewPublicKey, kNewPrivateKey,
                    CryptAuthKey::Status::kActive, KeyType::P256,
                    kFixedUserKeyPairHandle)}};
  RunKeyCreator(expected_new_keys, kClientEphemeralDh);

  // Timeout waiting for EnrollKeysResponse.
  EXPECT_TRUE(timer()->IsRunning());
  timer()->Fire();

  EXPECT_EQ(
      CryptAuthEnrollmentResult(CryptAuthEnrollmentResult::ResultCode::
                                    kErrorTimeoutWaitingForEnrollKeysResponse,
                                sync_keys_response.client_directive()),
      enrollment_result());
}

}  // namespace device_sync

}  // namespace chromeos
