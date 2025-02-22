// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/nqe/throughput_analyzer.h"

#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/containers/circular_deque.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/simple_test_tick_clock.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/default_tick_clock.h"
#include "net/dns/mock_host_resolver.h"
#include "net/log/test_net_log.h"
#include "net/nqe/network_quality_estimator.h"
#include "net/nqe/network_quality_estimator_params.h"
#include "net/nqe/network_quality_estimator_test_util.h"
#include "net/nqe/network_quality_estimator_util.h"
#include "net/test/test_with_scoped_task_environment.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

namespace nqe {

namespace {

class TestThroughputAnalyzer : public internal::ThroughputAnalyzer {
 public:
  TestThroughputAnalyzer(NetworkQualityEstimator* network_quality_estimator,
                         NetworkQualityEstimatorParams* params,
                         const base::TickClock* tick_clock)
      : internal::ThroughputAnalyzer(
            network_quality_estimator,
            params,
            base::ThreadTaskRunnerHandle::Get(),
            base::Bind(
                &TestThroughputAnalyzer::OnNewThroughputObservationAvailable,
                base::Unretained(this)),
            tick_clock,
            std::make_unique<BoundTestNetLog>()->bound()),
        throughput_observations_received_(0),
        bits_received_(0) {}

  ~TestThroughputAnalyzer() override = default;

  int32_t throughput_observations_received() const {
    return throughput_observations_received_;
  }

  void OnNewThroughputObservationAvailable(int32_t downstream_kbps) {
    throughput_observations_received_++;
  }

  int64_t GetBitsReceived() const override { return bits_received_; }

  void IncrementBitsReceived(int64_t additional_bits_received) {
    bits_received_ += additional_bits_received;
  }

  // Uses a mock resolver to force example.com to resolve to a public IP
  // address.
  void AddIPAddressResolution(TestURLRequestContext* context) {
    scoped_refptr<net::RuleBasedHostResolverProc> rules(
        new net::RuleBasedHostResolverProc(nullptr));
    // example1.com resolves to a public IP address.
    rules->AddRule("example.com", "27.0.0.3");
    mock_host_resolver_.set_rules(rules.get());
    context->set_host_resolver(&mock_host_resolver_);
  }

  using internal::ThroughputAnalyzer::disable_throughput_measurements;
  using internal::ThroughputAnalyzer::CountInFlightRequests;
  using internal::ThroughputAnalyzer::EraseHangingRequests;
  using internal::ThroughputAnalyzer::IsHangingWindow;

 private:
  int throughput_observations_received_;

  int64_t bits_received_;

  MockCachingHostResolver mock_host_resolver_;

  DISALLOW_COPY_AND_ASSIGN(TestThroughputAnalyzer);
};

using ThroughputAnalyzerTest = TestWithScopedTaskEnvironment;

TEST_F(ThroughputAnalyzerTest, MaximumRequests) {
  const struct {
    bool use_local_requests;
  } tests[] = {{
                   false,
               },
               {
                   true,
               }};

  for (const auto& test : tests) {
    const base::TickClock* tick_clock = base::DefaultTickClock::GetInstance();
    TestNetworkQualityEstimator network_quality_estimator;
    std::map<std::string, std::string> variation_params;
    NetworkQualityEstimatorParams params(variation_params);
    TestThroughputAnalyzer throughput_analyzer(&network_quality_estimator,
                                               &params, tick_clock);

    TestDelegate test_delegate;
    TestURLRequestContext context;
    throughput_analyzer.AddIPAddressResolution(&context);

    ASSERT_FALSE(throughput_analyzer.disable_throughput_measurements());
    base::circular_deque<std::unique_ptr<URLRequest>> requests;

    // Start more requests than the maximum number of requests that can be held
    // in the memory.
    const std::string url = test.use_local_requests
                                ? "http://127.0.0.1/test.html"
                                : "http://example.com/test.html";

    EXPECT_EQ(
        test.use_local_requests,
        nqe::internal::IsPrivateHost(
            context.host_resolver(),
            HostPortPair(GURL(url).host(), GURL(url).EffectiveIntPort())));
    for (size_t i = 0; i < 1000; ++i) {
      std::unique_ptr<URLRequest> request(
          context.CreateRequest(GURL(url), DEFAULT_PRIORITY, &test_delegate,
                                TRAFFIC_ANNOTATION_FOR_TESTS));
      throughput_analyzer.NotifyStartTransaction(*(request.get()));
      requests.push_back(std::move(request));
    }
    // Too many local requests should cause the |throughput_analyzer| to disable
    // throughput measurements.
    EXPECT_NE(test.use_local_requests,
              throughput_analyzer.IsCurrentlyTrackingThroughput());
  }
}

// Tests that the throughput observation is taken only if there are sufficient
// number of requests in-flight.
TEST_F(ThroughputAnalyzerTest, TestMinRequestsForThroughputSample) {
  const base::TickClock* tick_clock = base::DefaultTickClock::GetInstance();
  TestNetworkQualityEstimator network_quality_estimator;
  std::map<std::string, std::string> variation_params;
  variation_params["throughput_hanging_requests_cwnd_size_multiplier"] = "-1";
  NetworkQualityEstimatorParams params(variation_params);
  // Set HTTP RTT to a large value so that the throughput observation window
  // is not detected as hanging. In practice, this would be provided by
  // |network_quality_estimator| based on the recent observations.
  network_quality_estimator.SetStartTimeNullHttpRtt(
      base::TimeDelta::FromSeconds(100));

  for (size_t num_requests = 1;
       num_requests <= params.throughput_min_requests_in_flight() + 1;
       ++num_requests) {
    TestThroughputAnalyzer throughput_analyzer(&network_quality_estimator,
                                               &params, tick_clock);
    TestURLRequestContext context;
    throughput_analyzer.AddIPAddressResolution(&context);
    std::vector<std::unique_ptr<URLRequest>> requests_not_local;

    std::vector<TestDelegate> not_local_test_delegates(num_requests);
    for (size_t i = 0; i < num_requests; ++i) {
      // We don't care about completion, except for the first one (see below).
      not_local_test_delegates[i].set_on_complete(base::DoNothing());
      std::unique_ptr<URLRequest> request_not_local(context.CreateRequest(
          GURL("http://example.com/echo.html"), DEFAULT_PRIORITY,
          &not_local_test_delegates[i], TRAFFIC_ANNOTATION_FOR_TESTS));
      request_not_local->Start();
      requests_not_local.push_back(std::move(request_not_local));
    }

    not_local_test_delegates[0].RunUntilComplete();

    EXPECT_EQ(0, throughput_analyzer.throughput_observations_received());

    for (size_t i = 0; i < requests_not_local.size(); ++i) {
      throughput_analyzer.NotifyStartTransaction(*requests_not_local.at(i));
    }

    // Increment the bytes received count to emulate the bytes received for
    // |request_local| and |requests_not_local|.
    throughput_analyzer.IncrementBitsReceived(100 * 1000 * 8);

    for (size_t i = 0; i < requests_not_local.size(); ++i) {
      throughput_analyzer.NotifyRequestCompleted(*requests_not_local.at(i));
    }
    base::RunLoop().RunUntilIdle();

    int expected_throughput_observations =
        num_requests >= params.throughput_min_requests_in_flight() ? 1 : 0;
    EXPECT_EQ(expected_throughput_observations,
              throughput_analyzer.throughput_observations_received());
  }
}

// Tests that the hanging requests are dropped from the |requests_|, and
// throughput observation window is ended.
TEST_F(ThroughputAnalyzerTest, TestHangingRequests) {
  static const struct {
    int hanging_request_duration_http_rtt_multiplier;
    base::TimeDelta http_rtt;
    base::TimeDelta requests_hang_duration;
    bool expect_throughput_observation;
  } tests[] = {
      {
          // |requests_hang_duration| is less than 5 times the HTTP RTT.
          // Requests should not be marked as hanging.
          5, base::TimeDelta::FromMilliseconds(1000),
          base::TimeDelta::FromMilliseconds(3000), true,
      },
      {
          // |requests_hang_duration| is more than 5 times the HTTP RTT.
          // Requests should be marked as hanging.
          5, base::TimeDelta::FromMilliseconds(200),
          base::TimeDelta::FromMilliseconds(3000), false,
      },
      {
          // |requests_hang_duration| is less than
          // |hanging_request_min_duration_msec|. Requests should not be marked
          // as hanging.
          1, base::TimeDelta::FromMilliseconds(100),
          base::TimeDelta::FromMilliseconds(100), true,
      },
      {
          // |requests_hang_duration| is more than
          // |hanging_request_min_duration_msec|. Requests should be marked as
          // hanging.
          1, base::TimeDelta::FromMilliseconds(2000),
          base::TimeDelta::FromMilliseconds(3100), false,
      },
      {
          // |requests_hang_duration| is less than 5 times the HTTP RTT.
          // Requests should not be marked as hanging.
          5, base::TimeDelta::FromSeconds(2), base::TimeDelta::FromSeconds(1),
          true,
      },
      {
          // HTTP RTT is unavailable. Requests should not be marked as hanging.
          5, base::TimeDelta::FromSeconds(-1), base::TimeDelta::FromSeconds(-1),
          true,
      },
  };

  for (const auto& test : tests) {
    base::HistogramTester histogram_tester;
    const base::TickClock* tick_clock = base::DefaultTickClock::GetInstance();
    TestNetworkQualityEstimator network_quality_estimator;
    if (test.http_rtt >= base::TimeDelta())
      network_quality_estimator.SetStartTimeNullHttpRtt(test.http_rtt);
    std::map<std::string, std::string> variation_params;
    // Set the transport RTT multiplier to a large value so that the hanging
    // request decision is made only on the basis of the HTTP RTT.
    variation_params
        ["hanging_request_http_rtt_upper_bound_transport_rtt_multiplier"] =
            "10000";
    variation_params["throughput_hanging_requests_cwnd_size_multiplier"] = "-1";
    variation_params["hanging_request_duration_http_rtt_multiplier"] =
        base::NumberToString(test.hanging_request_duration_http_rtt_multiplier);

    NetworkQualityEstimatorParams params(variation_params);

    const size_t num_requests = params.throughput_min_requests_in_flight();
    TestThroughputAnalyzer throughput_analyzer(&network_quality_estimator,
                                               &params, tick_clock);
    TestURLRequestContext context;
    throughput_analyzer.AddIPAddressResolution(&context);
    std::vector<std::unique_ptr<URLRequest>> requests_not_local;

    std::vector<TestDelegate> not_local_test_delegates(num_requests);
    for (size_t i = 0; i < num_requests; ++i) {
      // We don't care about completion, except for the first one (see below).
      not_local_test_delegates[i].set_on_complete(base::DoNothing());
      std::unique_ptr<URLRequest> request_not_local(context.CreateRequest(
          GURL("http://example.com/echo.html"), DEFAULT_PRIORITY,
          &not_local_test_delegates[i], TRAFFIC_ANNOTATION_FOR_TESTS));
      request_not_local->Start();
      requests_not_local.push_back(std::move(request_not_local));
    }

    not_local_test_delegates[0].RunUntilComplete();

    EXPECT_EQ(0, throughput_analyzer.throughput_observations_received());

    for (size_t i = 0; i < num_requests; ++i) {
      throughput_analyzer.NotifyStartTransaction(*requests_not_local.at(i));
    }

    // Increment the bytes received count to emulate the bytes received for
    // |request_local| and |requests_not_local|.
    throughput_analyzer.IncrementBitsReceived(100 * 1000 * 8);

    // Mark in-flight requests as hanging requests (if specified in the test
    // params).
    if (test.requests_hang_duration >= base::TimeDelta())
      base::PlatformThread::Sleep(test.requests_hang_duration);

    EXPECT_EQ(num_requests, throughput_analyzer.CountInFlightRequests());

    for (size_t i = 0; i < num_requests; ++i) {
      throughput_analyzer.NotifyRequestCompleted(*requests_not_local.at(i));
      if (!test.expect_throughput_observation) {
        // All in-flight requests should be marked as hanging, and thus should
        // be deleted from the set of in-flight requests.
        EXPECT_EQ(0u, throughput_analyzer.CountInFlightRequests());
      } else {
        // One request should be deleted at one time.
        EXPECT_EQ(requests_not_local.size() - i - 1,
                  throughput_analyzer.CountInFlightRequests());
      }
    }

    base::RunLoop().RunUntilIdle();

    EXPECT_EQ(test.expect_throughput_observation,
              throughput_analyzer.throughput_observations_received() > 0);

    // Verify metrics recording.
    if (test.hanging_request_duration_http_rtt_multiplier < 0) {
      histogram_tester.ExpectTotalCount(
          "NQE.ThroughputAnalyzer.HangingRequests.Erased", 0);
      histogram_tester.ExpectTotalCount(
          "NQE.ThroughputAnalyzer.HangingRequests.NotErased", 0);
    } else {
      if (!test.expect_throughput_observation) {
        histogram_tester.ExpectBucketCount(
            "NQE.ThroughputAnalyzer.HangingRequests.Erased", num_requests, 1);
        // A sample is recorded everytime a request starts.
        histogram_tester.ExpectBucketCount(
            "NQE.ThroughputAnalyzer.HangingRequests.Erased", 0, num_requests);
        histogram_tester.ExpectTotalCount(
            "NQE.ThroughputAnalyzer.HangingRequests.Erased", num_requests + 1);

        histogram_tester.ExpectTotalCount(
            "NQE.ThroughputAnalyzer.HangingRequests.NotErased",
            num_requests + 1);
      } else {
        // One sample is recorded when request starts, and another when the
        // request completes.
        histogram_tester.ExpectUniqueSample(
            "NQE.ThroughputAnalyzer.HangingRequests.Erased", 0,
            num_requests * 2);
        histogram_tester.ExpectTotalCount(
            "NQE.ThroughputAnalyzer.HangingRequests.NotErased",
            num_requests * 2);
      }
    }
  }
}

// Tests that the check for hanging requests is done at most once per second.
TEST_F(ThroughputAnalyzerTest, TestHangingRequestsCheckedOnlyPeriodically) {
  base::SimpleTestTickClock tick_clock;

  TestNetworkQualityEstimator network_quality_estimator;
  network_quality_estimator.SetStartTimeNullHttpRtt(
      base::TimeDelta::FromSeconds(1));
  std::map<std::string, std::string> variation_params;
  variation_params["hanging_request_duration_http_rtt_multiplier"] = "5";
  variation_params["hanging_request_min_duration_msec"] = "2000";
  NetworkQualityEstimatorParams params(variation_params);

  TestThroughputAnalyzer throughput_analyzer(&network_quality_estimator,
                                             &params, &tick_clock);

  TestDelegate test_delegate;
  TestURLRequestContext context;
  throughput_analyzer.AddIPAddressResolution(&context);
  std::vector<std::unique_ptr<URLRequest>> requests_not_local;

  for (size_t i = 0; i < 2; ++i) {
    std::unique_ptr<URLRequest> request_not_local(context.CreateRequest(
        GURL("http://example.com/echo.html"), DEFAULT_PRIORITY, &test_delegate,
        TRAFFIC_ANNOTATION_FOR_TESTS));
    request_not_local->Start();
    requests_not_local.push_back(std::move(request_not_local));
  }

  std::unique_ptr<URLRequest> some_other_request(context.CreateRequest(
      GURL("http://example.com/echo.html"), DEFAULT_PRIORITY, &test_delegate,
      TRAFFIC_ANNOTATION_FOR_TESTS));

  test_delegate.RunUntilComplete();

  // First request starts at t=1. The second request starts at t=2. The first
  // request would be marked as hanging at t=6, and the second request at t=7
  // seconds.
  for (size_t i = 0; i < 2; ++i) {
    tick_clock.Advance(base::TimeDelta::FromMilliseconds(1000));
    throughput_analyzer.NotifyStartTransaction(*requests_not_local.at(i));
  }

  EXPECT_EQ(2u, throughput_analyzer.CountInFlightRequests());
  tick_clock.Advance(base::TimeDelta::FromMilliseconds(3500));
  // Current time is t = 5.5 seconds.
  throughput_analyzer.EraseHangingRequests(*some_other_request);
  EXPECT_EQ(2u, throughput_analyzer.CountInFlightRequests());

  tick_clock.Advance(base::TimeDelta::FromMilliseconds(1000));
  // Current time is t = 6.5 seconds.  One request should be marked as hanging.
  throughput_analyzer.EraseHangingRequests(*some_other_request);
  EXPECT_EQ(1u, throughput_analyzer.CountInFlightRequests());

  // Current time is t = 6.5 seconds. Calling NotifyBytesRead again should not
  // run the hanging request checker since the last check was at t=6.5 seconds.
  throughput_analyzer.EraseHangingRequests(*some_other_request);
  EXPECT_EQ(1u, throughput_analyzer.CountInFlightRequests());

  tick_clock.Advance(base::TimeDelta::FromMilliseconds(600));
  // Current time is t = 7.1 seconds. Calling NotifyBytesRead again should not
  // run the hanging request checker since the last check was at t=6.5 seconds
  // (less than 1 second ago).
  throughput_analyzer.EraseHangingRequests(*some_other_request);
  EXPECT_EQ(1u, throughput_analyzer.CountInFlightRequests());

  tick_clock.Advance(base::TimeDelta::FromMilliseconds(400));
  // Current time is t = 7.5 seconds. Calling NotifyBytesRead again should run
  // the hanging request checker since the last check was at t=6.5 seconds (at
  // least 1 second ago).
  throughput_analyzer.EraseHangingRequests(*some_other_request);
  EXPECT_EQ(0u, throughput_analyzer.CountInFlightRequests());
}

// Tests that the last received time for a request is updated when data is
// received for that request.
TEST_F(ThroughputAnalyzerTest, TestLastReceivedTimeIsUpdated) {
  base::SimpleTestTickClock tick_clock;

  TestNetworkQualityEstimator network_quality_estimator;
  network_quality_estimator.SetStartTimeNullHttpRtt(
      base::TimeDelta::FromSeconds(1));
  std::map<std::string, std::string> variation_params;
  variation_params["hanging_request_duration_http_rtt_multiplier"] = "5";
  variation_params["hanging_request_min_duration_msec"] = "2000";
  NetworkQualityEstimatorParams params(variation_params);

  TestThroughputAnalyzer throughput_analyzer(&network_quality_estimator,
                                             &params, &tick_clock);

  TestDelegate test_delegate;
  TestURLRequestContext context;
  throughput_analyzer.AddIPAddressResolution(&context);

  std::unique_ptr<URLRequest> request_not_local(context.CreateRequest(
      GURL("http://example.com/echo.html"), DEFAULT_PRIORITY, &test_delegate,
      TRAFFIC_ANNOTATION_FOR_TESTS));
  request_not_local->Start();

  test_delegate.RunUntilComplete();

  std::unique_ptr<URLRequest> some_other_request(context.CreateRequest(
      GURL("http://example.com/echo.html"), DEFAULT_PRIORITY, &test_delegate,
      TRAFFIC_ANNOTATION_FOR_TESTS));

  // Start time for the request is t=0 second. The request will be marked as
  // hanging at t=5 seconds.
  throughput_analyzer.NotifyStartTransaction(*request_not_local);

  tick_clock.Advance(base::TimeDelta::FromMilliseconds(4000));
  // Current time is t=4.0 seconds.

  throughput_analyzer.EraseHangingRequests(*some_other_request);
  EXPECT_EQ(1u, throughput_analyzer.CountInFlightRequests());

  //  The request will be marked as hanging at t=9 seconds.
  throughput_analyzer.NotifyBytesRead(*request_not_local);
  tick_clock.Advance(base::TimeDelta::FromMilliseconds(4000));
  // Current time is t=8 seconds.
  throughput_analyzer.EraseHangingRequests(*some_other_request);
  EXPECT_EQ(1u, throughput_analyzer.CountInFlightRequests());

  tick_clock.Advance(base::TimeDelta::FromMilliseconds(2000));
  // Current time is t=10 seconds.
  throughput_analyzer.EraseHangingRequests(*some_other_request);
  EXPECT_EQ(0u, throughput_analyzer.CountInFlightRequests());
}

// Test that a request that has been hanging for a long time is deleted
// immediately when EraseHangingRequests is called even if the last hanging
// request check was done recently.
TEST_F(ThroughputAnalyzerTest, TestRequestDeletedImmediately) {
  base::SimpleTestTickClock tick_clock;

  TestNetworkQualityEstimator network_quality_estimator;
  network_quality_estimator.SetStartTimeNullHttpRtt(
      base::TimeDelta::FromSeconds(1));
  std::map<std::string, std::string> variation_params;
  variation_params["hanging_request_duration_http_rtt_multiplier"] = "2";
  NetworkQualityEstimatorParams params(variation_params);

  TestThroughputAnalyzer throughput_analyzer(&network_quality_estimator,
                                             &params, &tick_clock);

  TestDelegate test_delegate;
  TestURLRequestContext context;
  throughput_analyzer.AddIPAddressResolution(&context);

  std::unique_ptr<URLRequest> request_not_local(context.CreateRequest(
      GURL("http://example.com/echo.html"), DEFAULT_PRIORITY, &test_delegate,
      TRAFFIC_ANNOTATION_FOR_TESTS));
  request_not_local->Start();

  test_delegate.RunUntilComplete();

  // Start time for the request is t=0 second. The request will be marked as
  // hanging at t=2 seconds.
  throughput_analyzer.NotifyStartTransaction(*request_not_local);
  EXPECT_EQ(1u, throughput_analyzer.CountInFlightRequests());

  tick_clock.Advance(base::TimeDelta::FromMilliseconds(2900));
  // Current time is t=2.9 seconds.

  throughput_analyzer.EraseHangingRequests(*request_not_local);
  EXPECT_EQ(1u, throughput_analyzer.CountInFlightRequests());

  // |request_not_local| should be deleted since it has been idle for 2.4
  // seconds.
  tick_clock.Advance(base::TimeDelta::FromMilliseconds(500));
  throughput_analyzer.NotifyBytesRead(*request_not_local);
  EXPECT_EQ(0u, throughput_analyzer.CountInFlightRequests());
}

// Tests if the throughput observation is taken correctly when local and network
// requests overlap.
TEST_F(ThroughputAnalyzerTest, TestThroughputWithMultipleRequestsOverlap) {
  static const struct {
    bool start_local_request;
    bool local_request_completes_first;
    bool expect_throughput_observation;
  } tests[] = {
      {
          false, false, true,
      },
      {
          true, false, false,
      },
      {
          true, true, true,
      },
  };

  for (const auto& test : tests) {
    const base::TickClock* tick_clock = base::DefaultTickClock::GetInstance();
    TestNetworkQualityEstimator network_quality_estimator;
    // Localhost requests are not allowed for estimation purposes.
    std::map<std::string, std::string> variation_params;
    variation_params["throughput_hanging_requests_cwnd_size_multiplier"] = "-1";
    NetworkQualityEstimatorParams params(variation_params);

    TestThroughputAnalyzer throughput_analyzer(&network_quality_estimator,
                                               &params, tick_clock);

    TestDelegate local_delegate;
    local_delegate.set_on_complete(base::DoNothing());
    TestURLRequestContext context;
    throughput_analyzer.AddIPAddressResolution(&context);
    std::unique_ptr<URLRequest> request_local;

    std::vector<std::unique_ptr<URLRequest>> requests_not_local;
    std::vector<TestDelegate> not_local_test_delegates(
        params.throughput_min_requests_in_flight());
    for (size_t i = 0; i < params.throughput_min_requests_in_flight(); ++i) {
      // We don't care about completion, except for the first one (see below).
      not_local_test_delegates[i].set_on_complete(base::DoNothing());
      std::unique_ptr<URLRequest> request_not_local(context.CreateRequest(
          GURL("http://example.com/echo.html"), DEFAULT_PRIORITY,
          &not_local_test_delegates[i], TRAFFIC_ANNOTATION_FOR_TESTS));
      request_not_local->Start();
      requests_not_local.push_back(std::move(request_not_local));
    }

    if (test.start_local_request) {
      request_local = context.CreateRequest(GURL("http://127.0.0.1/echo.html"),
                                            DEFAULT_PRIORITY, &local_delegate,
                                            TRAFFIC_ANNOTATION_FOR_TESTS);
      request_local->Start();
    }

    // Wait until the first not-local request completes.
    not_local_test_delegates[0].RunUntilComplete();

    EXPECT_EQ(0, throughput_analyzer.throughput_observations_received());

    // If |test.start_local_request| is true, then |request_local| starts
    // before |request_not_local|, and ends after |request_not_local|. Thus,
    // network quality estimator should not get a chance to record throughput
    // observation from |request_not_local| because of ongoing local request
    // at all times.
    if (test.start_local_request)
      throughput_analyzer.NotifyStartTransaction(*request_local);

    for (size_t i = 0; i < requests_not_local.size(); ++i) {
      throughput_analyzer.NotifyStartTransaction(*requests_not_local.at(i));
    }

    if (test.local_request_completes_first) {
      ASSERT_TRUE(test.start_local_request);
      throughput_analyzer.NotifyRequestCompleted(*request_local);
    }

    // Increment the bytes received count to emulate the bytes received for
    // |request_local| and |requests_not_local|.
    throughput_analyzer.IncrementBitsReceived(100 * 1000 * 8);

    for (size_t i = 0; i < requests_not_local.size(); ++i) {
      throughput_analyzer.NotifyRequestCompleted(*requests_not_local.at(i));
    }
    if (test.start_local_request && !test.local_request_completes_first)
      throughput_analyzer.NotifyRequestCompleted(*request_local);

    // Pump the message loop to let analyzer tasks get processed.
    base::RunLoop().RunUntilIdle();

    int expected_throughput_observations =
        test.expect_throughput_observation ? 1 : 0;
    EXPECT_EQ(expected_throughput_observations,
              throughput_analyzer.throughput_observations_received());
  }
}

// Tests if the throughput observation is taken correctly when two network
// requests overlap.
TEST_F(ThroughputAnalyzerTest, TestThroughputWithNetworkRequestsOverlap) {
  static const struct {
    size_t throughput_min_requests_in_flight;
    size_t number_requests_in_flight;
    int64_t increment_bits;
    bool expect_throughput_observation;
  } tests[] = {
      {
          1, 2, 100 * 1000 * 8, true,
      },
      {
          3, 1, 100 * 1000 * 8, false,
      },
      {
          3, 2, 100 * 1000 * 8, false,
      },
      {
          3, 3, 100 * 1000 * 8, true,
      },
      {
          3, 4, 100 * 1000 * 8, true,
      },
      {
          1, 2, 1, false,
      },
  };

  for (const auto& test : tests) {
    const base::TickClock* tick_clock = base::DefaultTickClock::GetInstance();
    TestNetworkQualityEstimator network_quality_estimator;
    // Localhost requests are not allowed for estimation purposes.
    std::map<std::string, std::string> variation_params;
    variation_params["throughput_min_requests_in_flight"] =
        base::NumberToString(test.throughput_min_requests_in_flight);
    variation_params["throughput_hanging_requests_cwnd_size_multiplier"] = "-1";
    NetworkQualityEstimatorParams params(variation_params);
    // Set HTTP RTT to a large value so that the throughput observation window
    // is not detected as hanging. In practice, this would be provided by
    // |network_quality_estimator| based on the recent observations.
    network_quality_estimator.SetStartTimeNullHttpRtt(
        base::TimeDelta::FromSeconds(100));

    TestThroughputAnalyzer throughput_analyzer(&network_quality_estimator,
                                               &params, tick_clock);
    TestURLRequestContext context;
    throughput_analyzer.AddIPAddressResolution(&context);

    EXPECT_EQ(0, throughput_analyzer.throughput_observations_received());

    std::vector<std::unique_ptr<URLRequest>> requests_in_flight;
    std::vector<TestDelegate> in_flight_test_delegates(
        test.number_requests_in_flight);
    for (size_t i = 0; i < test.number_requests_in_flight; ++i) {
      // We don't care about completion, except for the first one (see below).
      in_flight_test_delegates[i].set_on_complete(base::DoNothing());
      std::unique_ptr<URLRequest> request_network_1 = context.CreateRequest(
          GURL("http://example.com/echo.html"), DEFAULT_PRIORITY,
          &in_flight_test_delegates[i], TRAFFIC_ANNOTATION_FOR_TESTS);
      requests_in_flight.push_back(std::move(request_network_1));
      requests_in_flight.back()->Start();
    }

    in_flight_test_delegates[0].RunUntilComplete();

    EXPECT_EQ(0, throughput_analyzer.throughput_observations_received());

    for (size_t i = 0; i < test.number_requests_in_flight; ++i) {
      URLRequest* request = requests_in_flight.at(i).get();
      throughput_analyzer.NotifyStartTransaction(*request);
    }

    // Increment the bytes received count to emulate the bytes received for
    // |request_network_1| and |request_network_2|.
    throughput_analyzer.IncrementBitsReceived(test.increment_bits);

    for (size_t i = 0; i < test.number_requests_in_flight; ++i) {
      URLRequest* request = requests_in_flight.at(i).get();
      throughput_analyzer.NotifyRequestCompleted(*request);
    }

    base::RunLoop().RunUntilIdle();

    // Only one observation should be taken since two requests overlap.
    if (test.expect_throughput_observation) {
      EXPECT_EQ(1, throughput_analyzer.throughput_observations_received());
    } else {
      EXPECT_EQ(0, throughput_analyzer.throughput_observations_received());
    }
  }
}

// Tests if the throughput observation is taken correctly when the start and end
// of network requests overlap, and the minimum number of in flight requests
// when taking an observation is more than 1.
TEST_F(ThroughputAnalyzerTest, TestThroughputWithMultipleNetworkRequests) {
  const base::TickClock* tick_clock = base::DefaultTickClock::GetInstance();
  TestNetworkQualityEstimator network_quality_estimator;
  std::map<std::string, std::string> variation_params;
  variation_params["throughput_min_requests_in_flight"] = "3";
  variation_params["throughput_hanging_requests_cwnd_size_multiplier"] = "-1";
  NetworkQualityEstimatorParams params(variation_params);
  // Set HTTP RTT to a large value so that the throughput observation window
  // is not detected as hanging. In practice, this would be provided by
  // |network_quality_estimator| based on the recent observations.
  network_quality_estimator.SetStartTimeNullHttpRtt(
      base::TimeDelta::FromSeconds(100));

  TestThroughputAnalyzer throughput_analyzer(&network_quality_estimator,
                                             &params, tick_clock);
  TestDelegate test_delegate;
  TestURLRequestContext context;
  throughput_analyzer.AddIPAddressResolution(&context);

  EXPECT_EQ(0, throughput_analyzer.throughput_observations_received());

  std::unique_ptr<URLRequest> request_1 = context.CreateRequest(
      GURL("http://example.com/echo.html"), DEFAULT_PRIORITY, &test_delegate,
      TRAFFIC_ANNOTATION_FOR_TESTS);
  std::unique_ptr<URLRequest> request_2 = context.CreateRequest(
      GURL("http://example.com/echo.html"), DEFAULT_PRIORITY, &test_delegate,
      TRAFFIC_ANNOTATION_FOR_TESTS);
  std::unique_ptr<URLRequest> request_3 = context.CreateRequest(
      GURL("http://example.com/echo.html"), DEFAULT_PRIORITY, &test_delegate,
      TRAFFIC_ANNOTATION_FOR_TESTS);
  std::unique_ptr<URLRequest> request_4 = context.CreateRequest(
      GURL("http://example.com/echo.html"), DEFAULT_PRIORITY, &test_delegate,
      TRAFFIC_ANNOTATION_FOR_TESTS);

  request_1->Start();
  request_2->Start();
  request_3->Start();
  request_4->Start();

  // We dispatched four requests, so wait for four completions.
  for (int i = 0; i < 4; ++i)
    test_delegate.RunUntilComplete();

  EXPECT_EQ(0, throughput_analyzer.throughput_observations_received());

  throughput_analyzer.NotifyStartTransaction(*(request_1.get()));
  throughput_analyzer.NotifyStartTransaction(*(request_2.get()));

  const size_t increment_bits = 100 * 1000 * 8;

  // Increment the bytes received count to emulate the bytes received for
  // |request_1| and |request_2|.
  throughput_analyzer.IncrementBitsReceived(increment_bits);

  throughput_analyzer.NotifyRequestCompleted(*(request_1.get()));
  base::RunLoop().RunUntilIdle();

  // No observation should be taken since only 1 request is in flight.
  EXPECT_EQ(0, throughput_analyzer.throughput_observations_received());

  throughput_analyzer.NotifyStartTransaction(*(request_3.get()));
  throughput_analyzer.NotifyStartTransaction(*(request_4.get()));
  EXPECT_EQ(0, throughput_analyzer.throughput_observations_received());

  // 3 requests are in flight which is at least as many as the minimum number of
  // in flight requests required. An observation should be taken.
  throughput_analyzer.IncrementBitsReceived(increment_bits);

  // Only one observation should be taken since two requests overlap.
  throughput_analyzer.NotifyRequestCompleted(*(request_2.get()));
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(1, throughput_analyzer.throughput_observations_received());
  throughput_analyzer.NotifyRequestCompleted(*(request_3.get()));
  throughput_analyzer.NotifyRequestCompleted(*(request_4.get()));
  EXPECT_EQ(1, throughput_analyzer.throughput_observations_received());
}

TEST_F(ThroughputAnalyzerTest, TestHangingWindow) {
  static constexpr size_t kCwndSizeKilobytes = 10 * 1.5;
  static constexpr size_t kCwndSizeBits = kCwndSizeKilobytes * 1000 * 8;

  base::SimpleTestTickClock tick_clock;

  TestNetworkQualityEstimator network_quality_estimator;
  int64_t http_rtt_msec = 1000;
  network_quality_estimator.SetStartTimeNullHttpRtt(
      base::TimeDelta::FromMilliseconds(http_rtt_msec));
  std::map<std::string, std::string> variation_params;
  variation_params["throughput_hanging_requests_cwnd_size_multiplier"] = "1";
  NetworkQualityEstimatorParams params(variation_params);

  TestThroughputAnalyzer throughput_analyzer(&network_quality_estimator,
                                             &params, &tick_clock);

  const struct {
    size_t bits_received;
    base::TimeDelta window_duration;
    bool expected_hanging;
  } tests[] = {
      {100, base::TimeDelta::FromMilliseconds(http_rtt_msec), true},
      {kCwndSizeBits - 1, base::TimeDelta::FromMilliseconds(http_rtt_msec),
       true},
      {kCwndSizeBits + 1, base::TimeDelta::FromMilliseconds(http_rtt_msec),
       false},
      {2 * (kCwndSizeBits - 1),
       base::TimeDelta::FromMilliseconds(http_rtt_msec * 2), true},
      {2 * (kCwndSizeBits + 1),
       base::TimeDelta::FromMilliseconds(http_rtt_msec * 2), false},
      {kCwndSizeBits / 2 - 1,
       base::TimeDelta::FromMilliseconds(http_rtt_msec / 2), true},
      {kCwndSizeBits / 2 + 1,
       base::TimeDelta::FromMilliseconds(http_rtt_msec / 2), false},
  };

  for (const auto& test : tests) {
    base::HistogramTester histogram_tester;
    double kbps = test.bits_received / test.window_duration.InMillisecondsF();
    EXPECT_EQ(test.expected_hanging,
              throughput_analyzer.IsHangingWindow(test.bits_received,
                                                  test.window_duration, kbps));

    if (test.expected_hanging) {
      histogram_tester.ExpectUniqueSample("NQE.ThroughputObservation.Hanging",
                                          kbps, 1);
      histogram_tester.ExpectTotalCount("NQE.ThroughputObservation.NotHanging",
                                        0);
    } else {
      histogram_tester.ExpectTotalCount("NQE.ThroughputObservation.Hanging", 0);
      histogram_tester.ExpectUniqueSample(
          "NQE.ThroughputObservation.NotHanging", kbps, 1);
    }
  }
}

}  // namespace

}  // namespace nqe

}  // namespace net
