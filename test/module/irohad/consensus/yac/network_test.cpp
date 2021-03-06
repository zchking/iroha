/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "consensus/yac/transport/impl/network_impl.hpp"

#include <grpc++/grpc++.h>

#include "framework/test_logger.hpp"
#include "module/irohad/consensus/yac/mock_yac_crypto_provider.hpp"
#include "module/irohad/consensus/yac/mock_yac_network.hpp"
#include "module/irohad/consensus/yac/yac_test_util.hpp"

using ::testing::_;
using ::testing::DoAll;
using ::testing::InvokeWithoutArgs;
using ::testing::SaveArg;

namespace iroha {
  namespace consensus {
    namespace yac {
      class YacNetworkTest : public ::testing::Test {
       public:
        static constexpr auto default_ip = "127.0.0.1";
        static constexpr auto default_address = "127.0.0.1:0";
        void SetUp() override {
          notifications = std::make_shared<MockYacNetworkNotifications>();
          async_call = std::make_shared<
              network::AsyncGrpcClient<google::protobuf::Empty>>(
              getTestLogger("AsyncCall"));
          network = std::make_shared<NetworkImpl>(async_call,
                                                  getTestLogger("YacNetwork"));

          message.hash.vote_hashes.proposal_hash = "proposal";
          message.hash.vote_hashes.block_hash = "block";

          // getTransport is not used in network at the moment, please check if
          // test fails
          message.hash.block_signature = createSig("");
          message.signature = createSig("");
          network->subscribe(notifications);

          grpc::ServerBuilder builder;
          int port = 0;
          builder.AddListeningPort(
              default_address, grpc::InsecureServerCredentials(), &port);
          builder.RegisterService(network.get());
          server = builder.BuildAndStart();
          ASSERT_TRUE(server);
          ASSERT_NE(port, 0);

          peer = makePeer(std::string(default_ip) + ":" + std::to_string(port));
        }

        void TearDown() override {
          server->Shutdown();
        }

        std::shared_ptr<MockYacNetworkNotifications> notifications;
        std::shared_ptr<network::AsyncGrpcClient<google::protobuf::Empty>>
            async_call;
        std::shared_ptr<NetworkImpl> network;
        std::shared_ptr<shared_model::interface::Peer> peer;
        VoteMessage message;
        std::unique_ptr<grpc::Server> server;
        std::mutex mtx;
        std::condition_variable cv;
        shared_model::crypto::PublicKey pubkey =
            shared_model::crypto::PublicKey{""};
      };

      /**
       * @given initialized network
       * @when send vote to itself
       * @then vote handled
       */
      TEST_F(YacNetworkTest, MessageHandledWhenMessageSent) {
        bool processed = false;

        std::vector<VoteMessage> state;
        EXPECT_CALL(*notifications, onState(_))
            .Times(1)
            .WillRepeatedly(DoAll(SaveArg<0>(&state), InvokeWithoutArgs([&] {
                                    std::lock_guard<std::mutex> lock(mtx);
                                    processed = true;
                                    cv.notify_all();
                                  })));

        network->sendState(*peer, {message});

        // wait for response reader thread
        std::unique_lock<std::mutex> lk(mtx);
        ASSERT_TRUE(cv.wait_for(
            lk, std::chrono::seconds(5), [&] { return processed; }));

        ASSERT_EQ(1, state.size());
        ASSERT_EQ(message, state.front());
      }
    }  // namespace yac
  }    // namespace consensus
}  // namespace iroha
