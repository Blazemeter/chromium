// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "chrome/browser/chromeos/power/auto_screen_brightness/model_config_loader_impl.h"

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/task/task_scheduler/task_scheduler.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_task_environment.h"
#include "base/test/test_mock_time_task_runner.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "chromeos/constants/chromeos_features.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromeos {
namespace power {
namespace auto_screen_brightness {

namespace {

void CheckModelConfig(const ModelConfig& result, const ModelConfig& expected) {
  EXPECT_DOUBLE_EQ(result.auto_brightness_als_horizon_seconds,
                   expected.auto_brightness_als_horizon_seconds);
  EXPECT_EQ(result.log_lux.size(), expected.log_lux.size());
  for (size_t i = 0; i < result.log_lux.size(); ++i) {
    EXPECT_DOUBLE_EQ(result.log_lux[i], expected.log_lux[i]);
  }
  EXPECT_EQ(result.brightness.size(), expected.brightness.size());
  for (size_t i = 0; i < result.brightness.size(); ++i) {
    EXPECT_DOUBLE_EQ(result.brightness[i], expected.brightness[i]);
  }

  EXPECT_EQ(result.metrics_key, expected.metrics_key);
  EXPECT_DOUBLE_EQ(result.model_als_horizon_seconds,
                   expected.model_als_horizon_seconds);
}

class TestObserver : public ModelConfigLoader::Observer {
 public:
  TestObserver() {}
  ~TestObserver() override = default;

  // ModelConfigLoader::Observer overrides:
  void OnModelConfigLoaded(base::Optional<ModelConfig> model_config) override {
    model_config_loader_initialized_ = true;
    model_config_ = model_config;
  }

  bool model_config_loader_initialized() const {
    return model_config_loader_initialized_;
  }
  base::Optional<ModelConfig> model_config() { return model_config_; }

 private:
  bool model_config_loader_initialized_ = false;
  base::Optional<ModelConfig> model_config_;

  DISALLOW_COPY_AND_ASSIGN(TestObserver);
};

}  // namespace

class ModelConfigLoaderImplTest : public testing::Test {
 public:
  ModelConfigLoaderImplTest()
      : thread_bundle_(
            base::test::ScopedTaskEnvironment::MainThreadType::MOCK_TIME) {
    CHECK(temp_dir_.CreateUniqueTempDir());
    temp_params_path_ = temp_dir_.GetPath().Append("model_params.json");
  }

  ~ModelConfigLoaderImplTest() override {
    base::TaskScheduler::GetInstance()->FlushForTesting();
  }

  void Init(const std::string& model_params,
            const std::map<std::string, std::string>& experiment_params = {}) {
    base::test::ScopedFeatureList scoped_feature_list;
    if (!experiment_params.empty()) {
      scoped_feature_list.InitAndEnableFeatureWithParameters(
          features::kAutoScreenBrightness, experiment_params);
    }

    WriteParamsToFile(model_params);
    model_config_loader_ = ModelConfigLoaderImpl::CreateForTesting(
        temp_params_path_, base::SequencedTaskRunnerHandle::Get());

    test_observer_ = std::make_unique<TestObserver>();
    model_config_loader_->AddObserver(test_observer_.get());
    thread_bundle_.RunUntilIdle();
  }

 protected:
  void WriteParamsToFile(const std::string& params) {
    if (params.empty())
      return;

    CHECK(!temp_params_path_.empty());

    const int bytes_written =
        base::WriteFile(temp_params_path_, params.data(), params.size());
    ASSERT_EQ(bytes_written, static_cast<int>(params.size()))
        << "Wrote " << bytes_written << " byte(s) instead of " << params.size()
        << " to " << temp_params_path_;
  }

  content::TestBrowserThreadBundle thread_bundle_;

  base::ScopedTempDir temp_dir_;
  base::FilePath temp_params_path_;

  std::unique_ptr<ModelConfigLoaderImpl> model_config_loader_;
  std::unique_ptr<TestObserver> test_observer_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ModelConfigLoaderImplTest);
};

TEST_F(ModelConfigLoaderImplTest, ValidModelParamsLoaded) {
  const std::string model_params =
      "{\n"
      "  \"auto_brightness_als_horizon_seconds\": 2, \n"
      "  \"global_curve\": { \n"
      "  \"log_lux\": [ \n"
      "      1.0, \n"
      "      2.0, \n"
      "      3.0 \n"
      "    ], \n"
      "  \"brightness\": [ \n"
      "      10.0, \n"
      "      20.0, \n"
      "      30.0 \n"
      "    ] \n"
      "   }, \n"
      "  \"metrics_key\": \"abc\", \n"
      "  \"model_als_horizon_seconds\": 5 \n"
      "}\n";

  Init(model_params);
  EXPECT_TRUE(test_observer_->model_config_loader_initialized());

  std::vector<double> expected_log_lux = {1.0, 2.0, 3.0};
  std::vector<double> expected_brightness = {10.0, 20.0, 30.0};

  ModelConfig expected_model_config;
  expected_model_config.auto_brightness_als_horizon_seconds = 2.0;
  expected_model_config.log_lux = expected_log_lux;
  expected_model_config.brightness = expected_brightness;
  expected_model_config.metrics_key = "abc";
  expected_model_config.model_als_horizon_seconds = 5;
  EXPECT_TRUE(test_observer_->model_config());
  CheckModelConfig(*test_observer_->model_config(), expected_model_config);
}

TEST_F(ModelConfigLoaderImplTest, ValidModelParamsLoadedThenOverriden) {
  const std::string model_params =
      "{\n"
      "  \"auto_brightness_als_horizon_seconds\": 2, \n"
      "  \"global_curve\": { \n"
      "  \"log_lux\": [ \n"
      "      1.0, \n"
      "      2.0, \n"
      "      3.0 \n"
      "    ], \n"
      "  \"brightness\": [ \n"
      "      10.0, \n"
      "      20.0, \n"
      "      30.0 \n"
      "    ] \n"
      "   }, \n"
      "  \"metrics_key\": \"abc\", \n"
      "  \"model_als_horizon_seconds\": 5 \n"
      "}\n";

  const std::string global_curve_spec("2:20,4:40,6:60");

  const std::map<std::string, std::string> experiment_params = {
      {"auto_brightness_als_horizon_seconds", "10"},
      {"model_als_horizon_seconds", "20"},
      {"global_curve", global_curve_spec},
  };

  Init(model_params, experiment_params);
  EXPECT_TRUE(test_observer_->model_config_loader_initialized());

  std::vector<double> expected_log_lux = {2.0, 4.0, 6.0};
  std::vector<double> expected_brightness = {20.0, 40.0, 60.0};

  ModelConfig expected_model_config;
  expected_model_config.auto_brightness_als_horizon_seconds = 10.0;
  expected_model_config.log_lux = expected_log_lux;
  expected_model_config.brightness = expected_brightness;
  expected_model_config.metrics_key = "abc";
  expected_model_config.model_als_horizon_seconds = 20.0;
  EXPECT_TRUE(test_observer_->model_config());
  CheckModelConfig(*test_observer_->model_config(), expected_model_config);
}

TEST_F(ModelConfigLoaderImplTest, InvalidModelParamsLoaded) {
  // "auto_brightness_als_horizon_seconds" is missing.
  const std::string model_params =
      "{\n"
      "  \"global_curve\": { \n"
      "  \"log_lux\": [ \n"
      "      1.0, \n"
      "      2.0, \n"
      "      3.0 \n"
      "    ], \n"
      "  \"brightness\": [ \n"
      "      10.0, \n"
      "      20.0, \n"
      "      30.0 \n"
      "    ] \n"
      "   }, \n"
      "  \"metrics_key\": \"abc\", \n"
      "  \"model_als_horizon_seconds\": 5 \n"
      "}\n";

  Init(model_params);
  EXPECT_TRUE(test_observer_->model_config_loader_initialized());
  EXPECT_FALSE(test_observer_->model_config());
}

TEST_F(ModelConfigLoaderImplTest, InvalidModelParamsLoadedThenOverriden) {
  // Same as InvalidModelParamsLoaded, but missing
  // "auto_brightness_als_horizon_seconds" is specified in the experiment flags.
  const std::string model_params =
      "{\n"
      "  \"global_curve\": { \n"
      "  \"log_lux\": [ \n"
      "      1.0, \n"
      "      2.0, \n"
      "      3.0 \n"
      "    ], \n"
      "  \"brightness\": [ \n"
      "      10.0, \n"
      "      20.0, \n"
      "      30.0 \n"
      "    ] \n"
      "   }, \n"
      "  \"metrics_key\": \"abc\", \n"
      "  \"model_als_horizon_seconds\": 5 \n"
      "}\n";

  const std::map<std::string, std::string> experiment_params = {
      {"auto_brightness_als_horizon_seconds", "10"},
      {"model_als_horizon_seconds", "20"},
  };

  Init(model_params, experiment_params);
  EXPECT_TRUE(test_observer_->model_config_loader_initialized());

  std::vector<double> expected_log_lux = {1.0, 2.0, 3.0};
  std::vector<double> expected_brightness = {10.0, 20.0, 30.0};

  ModelConfig expected_model_config;
  expected_model_config.auto_brightness_als_horizon_seconds = 10.0;
  expected_model_config.log_lux = expected_log_lux;
  expected_model_config.brightness = expected_brightness;
  expected_model_config.metrics_key = "abc";
  expected_model_config.model_als_horizon_seconds = 20.0;
  EXPECT_TRUE(test_observer_->model_config());
  CheckModelConfig(*test_observer_->model_config(), expected_model_config);
}

TEST_F(ModelConfigLoaderImplTest, MissingModelParams) {
  // Model params not found from disk and experiment flags do not contain
  // all fields we need.
  const std::map<std::string, std::string> experiment_params = {
      {"auto_brightness_als_horizon_seconds", "10"},
      {"model_als_horizon_seconds", "20"},
  };

  Init("" /* model_params */, experiment_params);
  EXPECT_TRUE(test_observer_->model_config_loader_initialized());
  EXPECT_FALSE(test_observer_->model_config());
}

TEST_F(ModelConfigLoaderImplTest, InvalidJsonFormat) {
  const std::string model_params =
      "{\n"
      "  \"global_curve\": { \n"
      "  \"log_lux\": [ \n"
      "      1.0, \n"
      "      2.0, \n"
      "      3.0 \n"
      "    ], \n"
      "  \"brightness\": [ \n"
      "      10.0, \n"
      "      20.0, \n"
      "      30.0 \n"
      "    ] \n"
      "   }, \n"
      "  \"metrics_key\": 10, \n"
      "  \"model_als_horizon_seconds\": 5 \n"
      "}\n";

  const std::map<std::string, std::string> experiment_params = {
      {"auto_brightness_als_horizon_seconds", "10"},
      {"model_als_horizon_seconds", "20"},
  };

  Init(model_params, experiment_params);
  EXPECT_TRUE(test_observer_->model_config_loader_initialized());
  EXPECT_FALSE(test_observer_->model_config());
}

}  // namespace auto_screen_brightness
}  // namespace power
}  // namespace chromeos
